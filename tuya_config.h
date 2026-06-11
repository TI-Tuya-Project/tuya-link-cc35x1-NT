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

// #define TUYA_DEVICE_ID      "26e91ed3721e5b709cqyoh"
// #define TUYA_DEVICE_SECRET  "mVT47v6YlpVvuk27"

/* Optional, for reference / QR only — not used by tuya_mqtt_connect. */
// #define TUYA_PRODUCT_ID     "n7m2dxiulimpbmns"

#define TUYA_DEVICE_ID      "260036688bfaca6192zuwm"
#define TUYA_DEVICE_SECRET  "qhkYChAhhIkSwwP9"
#define TUYA_PRODUCT_ID     "crycb08pouebtrqe"


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

#endif /* TUYA_CONFIG_H */
