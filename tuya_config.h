/**
 * @file tuya_config.h
 * @brief Device identity + broker config for the pre-activated tuyalink path.
 *
 * Fill these with the credentials from the Tuya IoT Platform (Device -> "Free
 * Activation" / pre-registered device). NO product authkey/UUID and NO app
 * binding token are needed for the tuya_mqtt_* path — only device_id +
 * device_secret, which are HMAC-signed at CONNECT.
 *
 * Pick the broker host for the data-center the product lives in:
 *   China   : m1.tuyacn.com
 *   Western : m1.tuyaus.com
 *   Europe  : m1.tuyaeu.com
 *   India   : m1.tuyain.com
 * Port is 8883 (TLS) for all regions.
 */
#ifndef TUYA_CONFIG_H
#define TUYA_CONFIG_H

#define TUYA_DEVICE_ID      "2657e56a7c69256ccfekut"//"26a9ed09b1164478aaiyy7" OLD!!!!!
#define TUYA_DEVICE_SECRET  "Ni0uVIz9FhmpSZ15"//"5XJ2EK80wU5yZ5AX"

/* Optional, for reference / QR only — not used by tuya_mqtt_connect. */
#define TUYA_PRODUCT_ID     "spji157bithnbyc7"//"spji157bithnbyc7"

/* Final Switch DPs (must match Function Definition on the Tuya platform). */
#define TUYA_DP_LIGHT_COLOR_ID      101
#define TUYA_DP_LIGHT_COLOR_KEY     "light_color"

/* BTN-1 (left): increments on each press; app may set to 0 to reset.
 * Platform DP settings must use Scale=0 and Pitch(step)=1 so the app shows
 * whole numbers (Scale=1 makes each +1 display as +0.1). */
#define TUYA_DP_CLICK_COUNT_ID      102
#define TUYA_DP_CLICK_COUNT_KEY     "click_count"
#define TUYA_DP_CLICK_COUNT_MAX     9999

/* BTN-2 (right): pulse true for cross-device Smart Life automations. */
#define TUYA_DP_LINK_TRIGGER_ID     103
#define TUYA_DP_LINK_TRIGGER_KEY    "link_trigger"

/* The bundled CA (tuya_cacert.h) is GoDaddy Root G2, which signs Tuya's GLOBAL
 * brokers. Pick the broker for your Cloud project's data center. NOTE:
 * m1.tuyacn.com (China) chains to a different root and FAILS cert verify
 * (-0x2700) with this CA.
 *   Central Europe : m1.tuyaeu.com
 *   Western America: m1.tuyaus.com
 *   India          : m1.tuyain.com */
#define TUYA_MQTT_HOST      "m1.tuyaeu.com"
#define TUYA_MQTT_PORT      8883

/* MQTT keepalive (seconds) and per-IO TLS timeout (ms). */
#define TUYA_MQTT_KEEPALIVE 100
#define TUYA_MQTT_TIMEOUT_MS 5000

/* Default Wi-Fi for the UART `tuya` command — edit to change network. */
#define TUYA_WIFI_SSID              "Or_Bibi"
#define TUYA_WIFI_SECURITY          "WPA2"
#define TUYA_WIFI_PASSWORD          "19981998"
#define TUYA_CMD_STEP_DELAY_SEC     2

#endif /* TUYA_CONFIG_H */
