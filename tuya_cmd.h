#ifndef TUYA_CMD_H
#define TUYA_CMD_H

#include <stdint.h>

int32_t cmdTuyaCallback(void *arg);
int32_t printTuyaUsage(void *arg);

/** Run wlan_connect using TUYA_WIFI_* credentials from tuya_config.h. */
int32_t tuya_cmd_wlan_connect(void);

/** Kick off wlan_start + role up + connect after boot (non-blocking). */
void tuya_cmd_auto_start(void);

#endif /* TUYA_CMD_H */
