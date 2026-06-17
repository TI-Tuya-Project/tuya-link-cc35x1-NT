/**
 * @file tuya_app_task.c
 * @brief FreeRTOS task that brings the device online via the pre-activated
 *        tuyalink MQTT path: SNTP time sync -> tuya_mqtt_init -> connect -> loop.
 *
 * Prerequisite: Wi-Fi STA + IP must already be up (handled by the TI
 * network_terminal example). Call tuya_app_start() once the IP_EVENT /
 * IPV4_ACQUIRED has fired. This module deliberately does not own Wi-Fi.
 *
 * Modular I/O model
 * -----------------
 * Two tables map Tuya data points (DPs) to board GPIOs:
 *   - s_leds[]    : cloud -> board. A boolean DP write drives the LED output.
 *   - s_buttons[] : board -> cloud. A button level change is reported as a DP.
 * To add/rename an output or input, edit the table only. The DP `code` strings
 * MUST match the DP codes defined on the Tuya product, or the cloud will drop
 * the message.
 */
#include "tuya_app_task.h"
#include "tuya_config.h"
#include "tuya_sntp.h"
#include "ti_platform.h"

#include "tuya_log.h"
#include "tuya_error_code.h"
#include "tuyalink_core.h"

#include "FreeRTOS.h"
#include "task.h"

#include <ti/drivers/GPIO.h>
#include "ti_drivers_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Region CA root for the m1.tuya*.com brokers. Deployment-specific copy lives
 * in the app. */
#include "tuya_cacert.h"

#define TUYA_APP_TASK_STACK_WORDS  (4096)
#define TUYA_APP_TASK_PRIORITY     (3)

static tuya_mqtt_context_t s_client;

/* ----- DP <-> GPIO mapping (edit these two tables to add/rename I/O) ----- */

/* Outputs: a boolean DP write turns the LED on/off. */
typedef struct {
    const char *code;   /* Tuya DP code; must match the product definition */
    uint_least8_t gpio; /* CONFIG_GPIO_* index from ti_drivers_config.h */
} led_map_t;

static const led_map_t s_leds[] = {
    { "led_red",   CONFIG_GPIO_LED_0 },
    { "led_green", CONFIG_GPIO_LED_1 },
    { "led_blue",  CONFIG_GPIO_LED_2 },
};
#define LED_COUNT (sizeof(s_leds) / sizeof(s_leds[0]))

/* Inputs: sampled by a dedicated fast-polling task (button_poll_task) that runs
 * independently of the MQTT recv block, so quick presses are caught reliably.
 * Each debounced change is pushed onto a lock-free ring; the MQTT task drains it
 * and reports up as a boolean DP. (The CC35XX GPIO driver has no both-edges
 * interrupt, and single-edge toggling mis-handles fast taps, so we poll.)
 *
 * active_low: LaunchPad buttons read 0 when pressed (internal pull-up), so we
 * invert to report "pressed" as true. */
#define BTN_Q_LEN        8u                 /* power of two */
#define BTN_Q_MASK       (BTN_Q_LEN - 1u)
#define BTN_POLL_MS      10u                /* sample period */
#define BTN_STABLE_POLLS 2u                 /* consecutive equal samples to accept */

typedef struct {
    const char *code;
    uint_least8_t gpio;
    int active_low;
    /* SPSC ring: poll task is the sole producer (head), MQTT task the consumer (tail).
     * Each enqueued entry is a "press event" (1) -- the consumer toggles `reported`. */
    volatile uint8_t q[BTN_Q_LEN];
    volatile uint8_t head;
    volatile uint8_t tail;
    int last_logical;                       /* last accepted physical state (debounced) */
    int cand;                               /* candidate state being debounced */
    uint8_t cand_n;                         /* consecutive samples at cand */
    volatile int reported;                  /* latched toggle state pushed to cloud */
} button_map_t;

static button_map_t s_buttons[] = {
    { "button_1", CONFIG_GPIO_BTN_0, 1 },
    { "button_2", CONFIG_GPIO_BTN_1, 1 },
};
#define BUTTON_COUNT (sizeof(s_buttons) / sizeof(s_buttons[0]))

/* ----- Siren: alternate two external LEDs while an alarm DP is ON ----- */
#define SIREN_DP_CODE   "alarm"     /* boolean DP, Read/Write on the product */
#define SIREN_PERIOD_MS 150u        /* swap red<->blue every 150 ms */

static volatile int s_alarm_on = 0; /* set from inbound property/set */

/* ----- Helpers ----- */

/* Read a button's logical (pressed) state: 1 = pressed, 0 = released. */
static int button_read(const button_map_t *b)
{
    int level = GPIO_read(b->gpio);
    return b->active_low ? (level == 0) : (level != 0);
}

/* Push a debounced state change onto a button's ring (poll task is sole producer). */
static void button_enqueue(button_map_t *b, int state)
{
    if ((uint8_t)(b->head - b->tail) < BTN_Q_LEN) {
        b->q[b->head & BTN_Q_MASK] = (uint8_t)state;
        b->head = (uint8_t)(b->head + 1u);
    }
}

/* Dedicated task: sample every BTN_POLL_MS, accept a new state only after it has
 * held for BTN_STABLE_POLLS samples (debounce), then enqueue the change. Runs
 * independently of the MQTT recv block so fast presses are not missed. */
static void button_poll_task(void *arg)
{
    (void)arg;

    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        button_map_t *b = &s_buttons[i];
        b->head = 0;
        b->tail = 0;
        GPIO_setConfig(b->gpio, GPIO_CFG_IN_PU);
        b->last_logical = button_read(b);
        b->cand = b->last_logical;
        b->cand_n = 0;
    }

    for (;;) {
        for (size_t i = 0; i < BUTTON_COUNT; ++i) {
            button_map_t *b = &s_buttons[i];
            int s = button_read(b);
            if (s != b->cand) {
                b->cand = s;
                b->cand_n = 1;
            } else if (b->cand_n < BTN_STABLE_POLLS) {
                if (++b->cand_n >= BTN_STABLE_POLLS && s != b->last_logical) {
                    b->last_logical = s;
                    /* Only the press edge (released->pressed) is an event; the
                     * release is ignored. The consumer toggles `reported`. */
                    if (s) {
                        button_enqueue(b, 1);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

/* Start the button sampling task. GPIO is already initialized by Board_init(). */
static void buttons_start(void)
{
    xTaskCreate(button_poll_task, "tuya_btn", 512, NULL,
                TUYA_APP_TASK_PRIORITY, NULL);
}

/* Dedicated task: while s_alarm_on, flash the two external siren LEDs in
 * antiphase (red on/blue off <-> blue on/red off) every SIREN_PERIOD_MS and
 * pulse the active buzzer in step with the flash. When off, both LEDs and the
 * buzzer are held low. The pins are configured as outputs (initial low) by
 * Board_init() from SysConfig. */
static void siren_task(void *arg)
{
    (void)arg;
    int phase = 0;
    GPIO_write(CONFIG_GPIO_SIREN_RED, 0);
    GPIO_write(CONFIG_GPIO_SIREN_BLUE, 0);
    GPIO_write(CONFIG_GPIO_SIREN_BUZZER, 0);

    for (;;) {
        if (s_alarm_on) {
            phase = !phase;
            GPIO_write(CONFIG_GPIO_SIREN_RED, phase ? 1 : 0);
            GPIO_write(CONFIG_GPIO_SIREN_BLUE, phase ? 0 : 1);
            /* Beep on every red half-cycle: short on/off pulse train. */
            GPIO_write(CONFIG_GPIO_SIREN_BUZZER, phase ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(SIREN_PERIOD_MS));
        } else {
            if (phase) {
                GPIO_write(CONFIG_GPIO_SIREN_RED, 0);
                GPIO_write(CONFIG_GPIO_SIREN_BLUE, 0);
                GPIO_write(CONFIG_GPIO_SIREN_BUZZER, 0);
                phase = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void siren_start(void)
{
    xTaskCreate(siren_task, "tuya_siren", 512, NULL,
                TUYA_APP_TASK_PRIORITY, NULL);
}

/* Parse a flat boolean DP out of an inbound property/set payload, e.g.
 * {"led_red":true} or {"led_red":1}. Returns 1 if `code` was found and writes
 * the value to *on; returns 0 if absent. */
static int parse_bool_dp(const char *json, const char *code, int *on)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", code);
    const char *v = strstr(json, needle);
    if (v == NULL) {
        return 0;
    }
    v = strchr(v, ':');
    if (v == NULL) {
        return 0;
    }
    ++v;
    while (*v == ' ' || *v == '\t') {
        ++v;
    }
    if (strncmp(v, "true", 4) == 0) {
        *on = 1;
    } else if (strncmp(v, "false", 5) == 0) {
        *on = 0;
    } else {
        *on = (atoi(v) >= 1) ? 1 : 0;
    }
    return 1;
}

/* Report one boolean DP up to the cloud: {"<code>":{"value":<bool>}}. */
static void report_bool_dp(tuya_mqtt_context_t *context, const char *code, int on)
{
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"%s\":{\"value\":%s}}",
             code, on ? "true" : "false");
    tuyalink_thing_property_report(context, NULL, payload);
}

/* ----- Cloud -> board: apply an inbound property/set payload ----- */
static void handle_property_set(tuya_mqtt_context_t *context, const char *json)
{
    if (json == NULL) {
        return;
    }
    for (size_t i = 0; i < LED_COUNT; ++i) {
        int on;
        if (parse_bool_dp(json, s_leds[i].code, &on)) {
            GPIO_write(s_leds[i].gpio, on ? CONFIG_GPIO_LED_ON : CONFIG_GPIO_LED_OFF);
            TY_LOGI("LED %s -> %s", s_leds[i].code, on ? "ON" : "OFF");
            /* Echo state back so the app panel stays in sync. */
            report_bool_dp(context, s_leds[i].code, on);
        }
    }
    /* Keep the latched toggle state in sync if the app/server changes a button
     * DP directly, so the next physical press toggles from the right baseline. */
    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        int on;
        if (parse_bool_dp(json, s_buttons[i].code, &on)) {
            s_buttons[i].reported = on ? 1 : 0;
            TY_LOGI("BTN %s set by cloud -> %d", s_buttons[i].code, s_buttons[i].reported);
        }
    }
    /* Alarm/siren control DP: ON starts the red/blue flashing, OFF stops it. */
    {
        int on;
        if (parse_bool_dp(json, SIREN_DP_CODE, &on)) {
            s_alarm_on = on;
            TY_LOGI("SIREN %s", on ? "ON" : "OFF");
            report_bool_dp(context, SIREN_DP_CODE, on);
        }
    }
}

/* ----- board -> cloud: drain queued button edges and report them ----- */
static void drain_buttons(tuya_mqtt_context_t *context)
{
    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        button_map_t *b = &s_buttons[i];
        while (b->tail != b->head) {
            (void)b->q[b->tail & BTN_Q_MASK];   /* press event; payload unused */
            b->tail = (uint8_t)(b->tail + 1u);
            /* Each physical press toggles the latched state: pressed<->unpressed. */
            b->reported = !b->reported;
            TY_LOGI("BTN %s toggle -> %d", b->code, b->reported);
            report_bool_dp(context, b->code, b->reported);
        }
    }
}

static void on_connected(tuya_mqtt_context_t *context, void *user_data)
{
    (void)user_data;
    TY_LOGI("Tuya MQTT connected");

    /* Fetch the cloud-side data model, then publish the current state of every
     * I/O so the device shows online with correct values in the app. */
    tuyalink_thing_data_model_get(context, NULL);

    for (size_t i = 0; i < LED_COUNT; ++i) {
        int level = GPIO_read(s_leds[i].gpio);
        report_bool_dp(context, s_leds[i].code, level == CONFIG_GPIO_LED_ON);
    }
    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        report_bool_dp(context, s_buttons[i].code, s_buttons[i].reported);
    }
    report_bool_dp(context, SIREN_DP_CODE, s_alarm_on);
}

static void on_disconnect(tuya_mqtt_context_t *context, void *user_data)
{
    (void)context;
    (void)user_data;
    TY_LOGI("Tuya MQTT disconnected");
}

static void on_messages(tuya_mqtt_context_t *context, void *user_data,
                        const tuyalink_message_t *msg)
{
    (void)user_data;
    TY_LOGI("Tuya msg id:%s type:%d code:%d", msg->msgid, msg->type, msg->code);

    switch (msg->type) {
        case THING_TYPE_PROPERTY_SET:
            TY_LOGI("property set: %s", msg->data_string);
            handle_property_set(context, msg->data_string);
            break;
        case THING_TYPE_MODEL_RSP:
            TY_LOGI("model: %s", msg->data_string);
            break;
        default:
            break;
    }
}

static void tuya_app_task(void *arg)
{
    (void)arg;

    /* 1. Wall clock — required before the HMAC-signed CONNECT. */
    if (!tuya_platform_time_is_valid()) {
        tuya_sntp_sync(5);
    }
    if (!tuya_platform_time_is_valid()) {
        TY_LOGE("No valid time; aborting Tuya bring-up (CONNECT would be rejected)");
        vTaskDelete(NULL);
        return;
    }

    /* 2. Init the pre-activated MQTT client. */
    int ret = tuya_mqtt_init(&s_client, &(const tuya_mqtt_config_t){
        .host          = TUYA_MQTT_HOST,
        .port          = TUYA_MQTT_PORT,
        .cacert        = tuya_cacert_pem,
        .cacert_len    = sizeof(tuya_cacert_pem),
        .device_id     = TUYA_DEVICE_ID,
        .device_secret = TUYA_DEVICE_SECRET,
        .keepalive     = TUYA_MQTT_KEEPALIVE,
        .timeout_ms    = TUYA_MQTT_TIMEOUT_MS,
        .on_connected  = on_connected,
        .on_disconnect = on_disconnect,
        .on_messages   = on_messages,
    });
    if (ret != OPRT_OK) {
        TY_LOGE("tuya_mqtt_init failed: %d", ret);
        vTaskDelete(NULL);
        return;
    }

    /* 3. Connect (TLS + HMAC auth). Retry on transient failure. */
    while ((ret = tuya_mqtt_connect(&s_client)) != OPRT_OK) {
        TY_LOGE("tuya_mqtt_connect failed: %d, retrying in 5s", ret);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* 4. Start the button sampling and siren tasks now that we're connected. */
    buttons_start();
    siren_start();

    /* 5. Service the connection forever (keepalive, rx, callbacks) and flush
     *    any queued button edges captured by the ISR. */
    for (;;) {
        tuya_mqtt_loop(&s_client);
        drain_buttons(&s_client);
    }
}

int tuya_app_start(void)
{
    BaseType_t ok = xTaskCreate(tuya_app_task, "tuya_app",
                                TUYA_APP_TASK_STACK_WORDS, NULL,
                                TUYA_APP_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? 0 : -1;
}
