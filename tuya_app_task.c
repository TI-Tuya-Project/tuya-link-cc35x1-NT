/**
 * @file tuya_app_task.c
 * @brief TuyaLink MQTT task: light_color, click_count, link_trigger DPs.
 */
#include "tuya_app_task.h"
#include "tuya_app_buttons.h"
#include "tuya_app_lights.h"
#include "tuya_config.h"
#include "tuya_sntp.h"
#include "ti_platform.h"

#include "tuya_log.h"
#include "tuya_error_code.h"
#include "tuyalink_core.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "tuya_cacert.h"

#define TUYA_APP_TASK_STACK_WORDS  (4096)
#define TUYA_APP_TASK_PRIORITY     (3)

static tuya_mqtt_context_t s_client;
static volatile int s_mqtt_ready;
static QueueHandle_t s_click_report_q;

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

static const char *find_dp_value(const char *json, const char *key)
{
    char needle[32];
    const char *p;
    const char *v;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (p == NULL) {
        return NULL;
    }

    p = strchr(p + strlen(needle), ':');
    if (p == NULL) {
        return NULL;
    }
    p = skip_ws(p + 1);

    if (*p == '{') {
        v = strstr(p, "\"value\"");
        if (v == NULL) {
            return NULL;
        }
        p = strchr(v, ':');
        if (p == NULL) {
            return NULL;
        }
        return skip_ws(p + 1);
    }

    return p;
}

static int parse_string_dp(const char *json, const char *key,
                           char *out, size_t out_len)
{
    const char *p;
    const char *start;
    size_t len;

    p = find_dp_value(json, key);
    if (p == NULL || *p != '"') {
        return -1;
    }
    ++p;
    start = p;
    while (*p != '\0' && *p != '"') {
        ++p;
    }
    if (*p != '"') {
        return -1;
    }

    len = (size_t)(p - start);
    if (len + 1 > out_len) {
        return -1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static int parse_int_dp(const char *json, const char *key, int *out)
{
    const char *p;

    p = find_dp_value(json, key);
    if (p == NULL) {
        return -1;
    }

    *out = atoi(p);
    return 0;
}

static int parse_bool_dp(const char *json, const char *key, int *out_on)
{
    const char *p;

    p = find_dp_value(json, key);
    if (p == NULL) {
        return -1;
    }

    if (strncmp(p, "true", 4) == 0) {
        *out_on = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out_on = 0;
        return 0;
    }

    return -1;
}

int tuya_app_mqtt_is_ready(void)
{
    return s_mqtt_ready;
}

void tuya_app_report_light_color(void)
{
    char report[80];

    if (!s_mqtt_ready) {
        return;
    }
    snprintf(report, sizeof(report),
             "{\"" TUYA_DP_LIGHT_COLOR_KEY "\":{\"value\":\"%s\"}}",
             tuya_app_light_color_get());
    tuyalink_thing_property_report(&s_client, NULL, report);
}

void tuya_app_report_click_count(int count)
{
    char report[64];

    if (!s_mqtt_ready) {
        return;
    }
    snprintf(report, sizeof(report),
             "{\"" TUYA_DP_CLICK_COUNT_KEY "\":{\"value\":%d}}", count);
    if (tuyalink_thing_property_report(&s_client, NULL, report) != OPRT_OK) {
        TY_LOGW("click_count report failed for %d", count);
    }
}

void tuya_app_notify_click_count(int count)
{
    if (s_click_report_q == NULL) {
        return;
    }
    (void)xQueueOverwrite(s_click_report_q, &count);
}

static void flush_click_count_reports(void)
{
    int count;

    if (!s_mqtt_ready || s_click_report_q == NULL) {
        return;
    }

    do {
        int latest = -1;

        while (xQueueReceive(s_click_report_q, &count, 0) == pdTRUE) {
            latest = count;
        }
        if (latest < 0) {
            break;
        }
        tuya_app_report_click_count(latest);
    } while (uxQueueMessagesWaiting(s_click_report_q) > 0);
}

void tuya_app_report_link_trigger(int on)
{
    char report[64];

    if (!s_mqtt_ready) {
        return;
    }
    snprintf(report, sizeof(report),
             "{\"" TUYA_DP_LINK_TRIGGER_KEY "\":{\"value\":%s}}",
             on ? "true" : "false");
    tuyalink_thing_property_report(&s_client, NULL, report);
}

static void report_all_state(tuya_mqtt_context_t *context)
{
    (void)context;
    tuya_app_report_light_color();
    tuya_app_report_click_count(tuya_app_buttons_get_count());
    tuya_app_report_link_trigger(0);
}

static void handle_property_set(tuya_mqtt_context_t *context, const char *json)
{
    char color[16];
    int count;
    int trigger;
    int handled = 0;

    if (json == NULL) {
        return;
    }

    if (parse_string_dp(json, TUYA_DP_LIGHT_COLOR_KEY, color, sizeof(color)) == 0) {
        tuya_app_light_color_set(color);
        TY_LOGI("DP %s -> %s", TUYA_DP_LIGHT_COLOR_KEY, color);
        handled = 1;
    }

    if (parse_int_dp(json, TUYA_DP_CLICK_COUNT_KEY, &count) == 0) {
        tuya_app_buttons_set_count(count);
        TY_LOGI("DP %s -> %d", TUYA_DP_CLICK_COUNT_KEY, count);
        handled = 1;
    }

    if (parse_bool_dp(json, TUYA_DP_LINK_TRIGGER_KEY, &trigger) == 0) {
        TY_LOGI("DP %s -> %s", TUYA_DP_LINK_TRIGGER_KEY, trigger ? "true" : "false");
        handled = 1;
    }

    if (handled) {
        report_all_state(context);
    }
}

static void on_connected(tuya_mqtt_context_t *context, void *user_data)
{
    (void)user_data;
    TY_LOGI("Tuya MQTT connected");
    s_mqtt_ready = 1;

    tuyalink_thing_data_model_get(context, NULL);
    tuyalink_thing_desired_get(context, NULL,
                               "[\"" TUYA_DP_LIGHT_COLOR_KEY "\",\""
                               TUYA_DP_CLICK_COUNT_KEY "\",\""
                               TUYA_DP_LINK_TRIGGER_KEY "\"]");
    report_all_state(context);
}

static void on_disconnect(tuya_mqtt_context_t *context, void *user_data)
{
    (void)context;
    (void)user_data;
    s_mqtt_ready = 0;
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

    if (!tuya_platform_time_is_valid()) {
        tuya_sntp_sync(5);
    }
    if (!tuya_platform_time_is_valid()) {
        TY_LOGE("No valid time; aborting Tuya bring-up (CONNECT would be rejected)");
        vTaskDelete(NULL);
        return;
    }

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

    while ((ret = tuya_mqtt_connect(&s_client)) != OPRT_OK) {
        TY_LOGE("tuya_mqtt_connect failed: %d, retrying in 5s", ret);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    s_click_report_q = xQueueCreate(1, sizeof(int));
    if (s_click_report_q == NULL) {
        TY_LOGE("click_count report queue alloc failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        tuya_mqtt_loop(&s_client);
        flush_click_count_reports();
    }
}

int tuya_app_start(void)
{
    BaseType_t ok = xTaskCreate(tuya_app_task, "tuya_app",
                                TUYA_APP_TASK_STACK_WORDS, NULL,
                                TUYA_APP_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? 0 : -1;
}
