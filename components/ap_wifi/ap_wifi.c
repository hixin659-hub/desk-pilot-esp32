#include "ap_wifi.h"
#include "wifi_manager.h"
#include "esp_spiffs.h"
#include <sys/stat.h>
#include "string.h"
#include "ws_server.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "stdio.h"

#define TAG "ap_wifi"

#define SPIFF_MOUNT     "/spiffs"
#define HTML_PATH       "/spiffs/apcfg.html"

static char* html_code = NULL;

static char current_ssid[32] = {0};
static char current_password[64] = {0};

static EventGroupHandle_t apcfg_ev;
#define APCFG_BIT      (BIT0)

static char *init_web_page_buffer(void)
{
    esp_vfs_spiffs_conf_t conf =
    {
        .base_path = SPIFF_MOUNT,
        .format_if_mount_failed = false,
        .max_files = 3,
        .partition_label = NULL
    };
    esp_vfs_spiffs_register(&conf);
    struct stat st;
    if(stat(HTML_PATH, &st))
    {
        return NULL;
    }
    char* buf = (char*)malloc(st.st_size + 1);
    memset(buf, 0, st.st_size + 1);
    FILE* fp = fopen(HTML_PATH, "r");
    if(fp)
    {
        if(fread(buf, st.st_size, 1, fp) == 0)
        {
            free(buf);
            buf = NULL;
        }
        fclose(fp);     
    }
    else
    {
        free(buf);
        buf = NULL;
    }
    return buf;
}

static void ap_wifi_task(void* param)
{
    xEventGroupWaitBits(apcfg_ev, APCFG_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    wb_ws_stop();
    wifi_manager_connect(current_ssid, current_password);
    vTaskDelete(NULL);
}

void ap_wifi_init(p_wifi_state_cb f)
{
    wifi_manager_init(f);
    html_code = init_web_page_buffer();
    apcfg_ev = xEventGroupCreate();
    xTaskCreatePinnedToCore(ap_wifi_task, "apcfg", 4096, NULL, 3, NULL, 1);
}

void wifi_scan_handle(int num, wifi_ap_record_t *ap_records)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* wifilist_js = cJSON_AddArrayToObject(root, "wifi_list");
    for(int i = 0; i < num; i++)
    {
        cJSON* wifi_js = cJSON_CreateObject();
        cJSON_AddStringToObject(wifi_js, "ssid", (char*)ap_records[i].ssid);
        cJSON_AddNumberToObject(wifi_js, "rssi", ap_records[i].rssi);
       if(ap_records[i].authmode == WIFI_AUTH_OPEN)
            cJSON_AddBoolToObject(wifi_js, "encrypted", 0);
        else
            cJSON_AddBoolToObject(wifi_js, "encrypted", 1);
        cJSON_AddItemToArray(wifilist_js, wifi_js);
    }
    char* data = cJSON_PrintUnformatted(root);
    esp_err_t ret = wb_ws_send((uint8_t*)data, strlen(data));
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WS send failed: %d", ret);
    }
    cJSON_free(data);
    cJSON_Delete(root);
}

static void ws_receive_handle(uint8_t* payload, int len)
{
    cJSON* root = cJSON_Parse((char*)payload);
    if(root)
    {
        cJSON* scan_js = cJSON_GetObjectItem(root, "scan");
        cJSON* ssid_js = cJSON_GetObjectItem(root, "ssid");
        cJSON* password_js = cJSON_GetObjectItem(root, "password");
        if(scan_js)
        {
            char* scan_value = cJSON_GetStringValue(scan_js);
            if(strcmp(scan_value, "start") == 0)
            {
                wifi_manager_scan(wifi_scan_handle);
            }
        }
        if(ssid_js && password_js)
        {
            char* ssid_value = cJSON_GetStringValue(ssid_js);
            char* password_value = cJSON_GetStringValue(password_js);
            snprintf(current_ssid, sizeof(current_ssid), "%s", ssid_value);
            snprintf(current_password, sizeof(current_password), "%s", password_value);
            xEventGroupSetBits(apcfg_ev, APCFG_BIT);
        }
    }
}

//进入配网模式
void ap_wifi_apcfg(void)
{
    wifi_manager_ap();
    ws_cfg_t ws_cfg =
    {
        .html_code = html_code,
        .receive_fn = ws_receive_handle,
    };
    wb_ws_start(&ws_cfg);
}
