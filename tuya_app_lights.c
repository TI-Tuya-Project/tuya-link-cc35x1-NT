/**
 * @file tuya_app_lights.c
 * @brief LP_EM_CC35X1 RGB LED control for Tuya light_color enum DP.
 */
#include "tuya_app_lights.h"

#include "ti_drivers_config.h"
#include <ti/drivers/GPIO.h>

#include <string.h>

#define LIGHT_COLOR_OFF   "off"
#define LIGHT_COLOR_RED   "red"
#define LIGHT_COLOR_GREEN "green"
#define LIGHT_COLOR_BLUE  "blue"

static const uint_least8_t s_led_red   = CONFIG_GPIO_LED_0;
static const uint_least8_t s_led_green = CONFIG_GPIO_LED_1;
static const uint_least8_t s_led_blue  = CONFIG_GPIO_LED_2;

static char s_current[8] = LIGHT_COLOR_OFF;

static void all_leds_off(void)
{
    GPIO_write(s_led_red,   CONFIG_GPIO_LED_OFF);
    GPIO_write(s_led_green, CONFIG_GPIO_LED_OFF);
    GPIO_write(s_led_blue,  CONFIG_GPIO_LED_OFF);
}

void tuya_app_lights_init(void)
{
    strncpy(s_current, LIGHT_COLOR_OFF, sizeof(s_current));
    s_current[sizeof(s_current) - 1] = '\0';
    all_leds_off();
}

void tuya_app_light_color_set(const char *color)
{
    if (color == NULL) {
        return;
    }

    all_leds_off();

    if (strcmp(color, LIGHT_COLOR_RED) == 0) {
        GPIO_write(s_led_red, CONFIG_GPIO_LED_ON);
        strncpy(s_current, LIGHT_COLOR_RED, sizeof(s_current));
    } else if (strcmp(color, LIGHT_COLOR_GREEN) == 0) {
        GPIO_write(s_led_green, CONFIG_GPIO_LED_ON);
        strncpy(s_current, LIGHT_COLOR_GREEN, sizeof(s_current));
    } else if (strcmp(color, LIGHT_COLOR_BLUE) == 0) {
        GPIO_write(s_led_blue, CONFIG_GPIO_LED_ON);
        strncpy(s_current, LIGHT_COLOR_BLUE, sizeof(s_current));
    } else {
        strncpy(s_current, LIGHT_COLOR_OFF, sizeof(s_current));
    }

    s_current[sizeof(s_current) - 1] = '\0';
}

const char *tuya_app_light_color_get(void)
{
    return s_current;
}
