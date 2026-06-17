/**
 * @file tuya_app_task.h
 * @brief Tuya MQTT bring-up and cloud property reporting.
 */
#ifndef TUYA_APP_TASK_H
#define TUYA_APP_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

int tuya_app_start(void);

/** True after the first successful MQTT connect. */
int tuya_app_mqtt_is_ready(void);

void tuya_app_report_light_color(void);
void tuya_app_report_click_count(int count);
/** Queue latest count for reporting from the MQTT task (safe for rapid button presses). */
void tuya_app_notify_click_count(int count);
void tuya_app_report_link_trigger(int on);

#ifdef __cplusplus
}
#endif

#endif /* TUYA_APP_TASK_H */
