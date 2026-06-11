/**
 * @file tuya_app_task.c
 * @brief FreeRTOS task that brings the device online via the pre-activated
 *        tuyalink MQTT path: SNTP time sync -> tuya_mqtt_init -> connect -> loop.
 *
 * Prerequisite: Wi-Fi STA + IP must already be up (handled by the TI
 * network_terminal example). Call tuya_app_start() once the IP_EVENT /
 * IPV4_ACQUIRED has fired. This module deliberately does not own Wi-Fi.
 *
 * Reporting: edit on_connected() / the example data point ids to match your
 * product's DP schema on the Tuya platform.
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

#include <string.h>
#include <stdlib.h>

/* Region CA root for the m1.tuya*.com brokers. Provided per-example in the
 * upstream SDK; the data_model demo's copy is reused here. */
#include "tuya_cacert.h"

#define TUYA_APP_TASK_STACK_WORDS  (4096)
#define TUYA_APP_TASK_PRIORITY     (3)

static tuya_mqtt_context_t s_client;

/* Weak default — the application overrides this to drive real board hardware.
 * Kept in the library so the bring-up code links standalone. */
__attribute__((weak)) void tuya_app_led_set(int on)
{
    TY_LOGI("tuya_app_led_set(%d) [no-op: override in app]", on);
}

/* Map an inbound property/set payload to an on/off and drive the LED hook.
 * Boolean DP  -> true/false.
 * Numeric DP  -> on when value >= 80 (works with the demo "upload" 60..100 DP;
 *                add a Boolean DP for a real switch with a clean off state). */
static void handle_property_set(tuya_mqtt_context_t *context, const char *json)
{
    if (json == NULL) {
        return;
    }
    const char *v = strstr(json, "\"switch\"");
    int on = -1;
    if (v != NULL) {
        v = strchr(v, ':');
        if (v != NULL) {
            ++v;
            while (*v == ' ' || *v == '\t') {
                ++v;
            }
            if (strncmp(v, "true", 4) == 0) {
                on = 1;
            } else if (strncmp(v, "false", 5) == 0) {
                on = 0;
            } else {
                on = (atoi(v) >= 1) ? 1 : 0;
            }
        }
    }
    if (on >= 0) {
        tuya_app_led_set(on);
        /* Echo the new state back so the app panel stays in sync. */
        tuyalink_thing_property_report(context, NULL, json);
    }
}

static void on_connected(tuya_mqtt_context_t *context, void *user_data)
{
    (void)user_data;
    TY_LOGI("Tuya MQTT connected");

    /* Fetch the cloud-side data model and current desired values, then push an
     * initial property so the device shows as online with state in Smart Life.
     * DP "upload": integer, range 60..100 (abilityId 101) per this product. */
    tuyalink_thing_data_model_get(context, NULL);
    tuyalink_thing_property_report(context, NULL, "{\"switch\":{\"value\":true}}");
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
    (void)context;
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

    /* 4. Service the connection forever (keepalive, rx, callbacks). */
    for (;;) {
        tuya_mqtt_loop(&s_client);
    }
}

int tuya_app_start(void)
{
    BaseType_t ok = xTaskCreate(tuya_app_task, "tuya_app",
                                TUYA_APP_TASK_STACK_WORDS, NULL,
                                TUYA_APP_TASK_PRIORITY, NULL);
    return (ok == pdPASS) ? 0 : -1;
}
