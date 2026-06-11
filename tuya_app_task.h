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

#ifdef __cplusplus
}
#endif

#endif /* TUYA_APP_TASK_H */
