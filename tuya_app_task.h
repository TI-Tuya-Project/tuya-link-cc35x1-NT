/**
 * @file tuya_app_task.h
 * @brief Entry point for the Tuya bring-up task.
 */
#ifndef TUYA_APP_TASK_H
#define TUYA_APP_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the FreeRTOS task that syncs time and connects to Tuya cloud.
 *
 * Call once, after Wi-Fi STA + IPv4 are up. Non-blocking.
 *
 * @return 0 if the task was created, negative otherwise.
 */
int tuya_app_start(void);

/**
 * @brief Drive a board output from an inbound cloud DP command.
 *
 * Weak no-op default lives in tuya_app_task.c. The application (which owns the
 * board GPIO config) overrides this with a strong definition, e.g.
 * GPIO_write(CONFIG_GPIO_LED_0, on).
 *
 * @param on 1 = on, 0 = off.
 */
void tuya_app_led_set(int on);

#ifdef __cplusplus
}
#endif

#endif /* TUYA_APP_TASK_H */
