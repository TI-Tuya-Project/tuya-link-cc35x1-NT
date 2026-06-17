/**
 * @file tuya_app_buttons.h
 * @brief LaunchPad BTN-1 (counter) and BTN-2 (link trigger) handling.
 */
#ifndef TUYA_APP_BUTTONS_H
#define TUYA_APP_BUTTONS_H

#ifdef __cplusplus
extern "C" {
#endif

void tuya_app_buttons_init(void);

/** Current click counter (also resettable from the app via click_count DP). */
int tuya_app_buttons_get_count(void);
void tuya_app_buttons_set_count(int count);

#ifdef __cplusplus
}
#endif

#endif /* TUYA_APP_BUTTONS_H */
