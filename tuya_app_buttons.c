/**
 * @file tuya_app_buttons.c
 * @brief BTN-1 increments click_count; BTN-2 pulses link_trigger for automations.
 */
#include "tuya_app_buttons.h"
#include "tuya_app_task.h"
#include "tuya_config.h"

#include "ti_drivers_config.h"
#include <ti/drivers/GPIO.h>

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

#define BTN_POLL_MS           20
#define BTN_DEBOUNCE_MS       50
#define LINK_PULSE_MS         300

static int s_click_count;
static TaskHandle_t s_btn_task;

static int btn_pressed(uint_least8_t pin)
{
    return GPIO_read(pin) == 1;
}

static void handle_btn0_press(void)
{
    if (s_click_count < TUYA_DP_CLICK_COUNT_MAX) {
        s_click_count++;
    }
    if (tuya_app_mqtt_is_ready()) {
        tuya_app_notify_click_count(s_click_count);
    }
    printf("[TUYA] BTN-1 click_count -> %d\n\r", s_click_count);
}

static void handle_btn1_press(void)
{
    if (!tuya_app_mqtt_is_ready()) {
        printf("[TUYA] BTN-2 ignored (MQTT not ready)\n\r");
        return;
    }
    tuya_app_report_link_trigger(1);
    printf("[TUYA] BTN-2 link_trigger -> true\n\r");
    vTaskDelay(pdMS_TO_TICKS(LINK_PULSE_MS));
    tuya_app_report_link_trigger(0);
    printf("[TUYA] BTN-2 link_trigger -> false\n\r");
}

static void button_task(void *arg)
{
    int btn0_last = 0, btn0_stable = 0, btn0_armed = 1;
    int btn1_last = 0, btn1_stable = 0, btn1_armed = 1;

    (void)arg;

    for (;;) {
        int raw0 = btn_pressed(CONFIG_GPIO_BTN_0);
        int raw1 = btn_pressed(CONFIG_GPIO_BTN_1);

        if (raw0 != btn0_last) {
            btn0_stable = 0;
            btn0_last = raw0;
        } else if (raw0) {
            if (btn0_stable < (BTN_DEBOUNCE_MS / BTN_POLL_MS)) {
                btn0_stable++;
            } else if (btn0_armed) {
                handle_btn0_press();
                btn0_armed = 0;
            }
        } else {
            btn0_stable = 0;
            btn0_armed = 1;
        }

        if (raw1 != btn1_last) {
            btn1_stable = 0;
            btn1_last = raw1;
        } else if (raw1) {
            if (btn1_stable < (BTN_DEBOUNCE_MS / BTN_POLL_MS)) {
                btn1_stable++;
            } else if (btn1_armed) {
                handle_btn1_press();
                btn1_armed = 0;
            }
        } else {
            btn1_stable = 0;
            btn1_armed = 1;
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

void tuya_app_buttons_init(void)
{
    if (s_btn_task != NULL) {
        return;
    }
    xTaskCreate(button_task, "tuya_btn", 512, NULL, 2, &s_btn_task);
}

int tuya_app_buttons_get_count(void)
{
    return s_click_count;
}

void tuya_app_buttons_set_count(int count)
{
    if (count < 0) {
        count = 0;
    }
    if (count > TUYA_DP_CLICK_COUNT_MAX) {
        count = TUYA_DP_CLICK_COUNT_MAX;
    }
    s_click_count = count;
}
