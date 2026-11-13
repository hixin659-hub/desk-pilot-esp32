#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

typedef enum 
{
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
} WIFI_STATE;

typedef void(*p_wifi_state_cb)(WIFI_STATE state);
typedef void(*p_wifi_scan_cb)(int num, wifi_ap_record_t *ap_records);

void wifi_manager_init(p_wifi_state_cb f);

void wifi_manager_connect(const char *ssid, const char *password );

esp_err_t wifi_manager_ap(void);

esp_err_t wifi_manager_scan(p_wifi_scan_cb f);

/**
 * @brief 获取当前STA是否已联网（已连接AP并获取到IP）
 *
 * @return true  已联网
 * @return false 未联网
 */
bool wifi_manager_is_connected(void);


#endif
