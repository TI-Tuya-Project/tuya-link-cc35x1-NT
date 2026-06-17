/**
 * @file tuya_app_lights.h
 * @brief Maps Tuya light_color enum DP to the LP_EM_CC35X1 RGB LaunchPad LEDs.
 */
#ifndef TUYA_APP_LIGHTS_H
#define TUYA_APP_LIGHTS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Turn all RGB LEDs off (called after Board_init()).
 */
void tuya_app_lights_init(void);

/**
 * @brief Apply enum value: "off", "red", "green", or "blue".
 *
 * Turns all LEDs off, then lights the matching color (if not "off").
 */
void tuya_app_light_color_set(const char *color);

/**
 * @brief Current color string ("off", "red", "green", or "blue").
 */
const char *tuya_app_light_color_get(void);

#ifdef __cplusplus
}
#endif

#endif /* TUYA_APP_LIGHTS_H */
