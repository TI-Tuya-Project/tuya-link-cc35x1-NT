/**
 * @file tuya_cmd.c
 * @brief UART `tuya` command and auto Wi-Fi bring-up on boot.
 */
#include "tuya_cmd.h"
#include "tuya_config.h"
#include "network_terminal.h"
#include "wlan_cmd.h"
#include "uart_term.h"
#include "osi_kernel.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#define TUYA_AUTO_TASK_STACK_WORDS   4096
#define TUYA_AUTO_TASK_PRIORITY      2

static void tuya_cmd_delay(void)
{
    os_sleep(TUYA_CMD_STEP_DELAY_SEC, 0);
}

static int tuya_cmd_build_connect_arg(char *buf, size_t len)
{
    int ret = snprintf(buf, len,
                       " -s \"%s\" -t %s -p \"%s\"",
                       TUYA_WIFI_SSID, TUYA_WIFI_SECURITY, TUYA_WIFI_PASSWORD);
    if (ret < 0 || (size_t)ret >= len) {
        return -1;
    }
    return 0;
}

int32_t tuya_cmd_wlan_connect(void)
{
    char connectArg[CMD_BUFFER_LEN];
    int32_t ret;

    if (tuya_cmd_build_connect_arg(connectArg, sizeof(connectArg)) != 0) {
        UART_PRINT("[TUYA] connect argument too long\n\r");
        return -1;
    }

    UART_PRINT("\n\r[TUYA] wlan_connect -s \"%s\"\n\r", TUYA_WIFI_SSID);
    ret = cmdWlanConnectCallback(connectArg);
    if (ret != 0) {
        UART_PRINT("[TUYA] wlan_connect failed (%d)\n\r", ret);
    }
    return ret;
}

static void auto_tuya_task(void *arg)
{
    (void)arg;

    os_sleep(1, 0);
    UART_PRINT("\n\r[TUYA] Auto Wi-Fi bring-up starting...\n\r");
    (void)cmdTuyaCallback(NULL);
    vTaskDelete(NULL);
}

void tuya_cmd_auto_start(void)
{
    if (xTaskCreate(auto_tuya_task, "tuya_auto",
                    TUYA_AUTO_TASK_STACK_WORDS, NULL,
                    TUYA_AUTO_TASK_PRIORITY, NULL) != pdPASS) {
        UART_PRINT("[TUYA] auto bring-up task create failed\n\r");
    }
}

int32_t cmdTuyaCallback(void *arg)
{
    int32_t ret;

    (void)arg;

    UART_PRINT("\n\r[TUYA] Step 1/3: wlan_start\n\r");
    ret = cmdWlanStartCallback(" ");
    if (ret != 0) {
        UART_PRINT("[TUYA] wlan_start returned %d (may already be up)\n\r", ret);
    }
    tuya_cmd_delay();

    UART_PRINT("\n\r[TUYA] Step 2/3: wlan_sta_role_up\n\r");
    ret = cmdWlanRoleUpStaCallback(" ");
    if (ret != 0) {
        UART_PRINT("[TUYA] wlan_sta_role_up failed (%d)\n\r", ret);
        return ret;
    }
    tuya_cmd_delay();

    UART_PRINT("\n\r[TUYA] Step 3/3: wlan_connect -s \"%s\"\n\r", TUYA_WIFI_SSID);
    ret = tuya_cmd_wlan_connect();
    if (ret != 0) {
        return ret;
    }

    UART_PRINT("\n\r[TUYA] Wi-Fi setup done. MQTT starts when IP is acquired.\n\r");
    return 0;
}

int32_t printTuyaUsage(void *arg)
{
    (void)arg;
    UART_PRINT(lineBreak);
    UART_PRINT(usageStr);
    UART_PRINT(tuyaStr);
    UART_PRINT(tuyaUsageStr);
    UART_PRINT(descriptionStr);
    UART_PRINT(tuyaDetailsStr);
    UART_PRINT(lineBreak);
    return 0;
}
