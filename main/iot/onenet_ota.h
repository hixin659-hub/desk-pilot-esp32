#ifndef _ONENET_OTA_H_
#define _ONENET_OTA_H_

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    ONENET_OTA_EVT_CHECKING = 0,
    ONENET_OTA_EVT_VERSION_UPLOADED, 
    ONENET_OTA_EVT_NO_UPDATE,
    ONENET_OTA_EVT_UPDATE_FOUND,
    ONENET_OTA_EVT_DOWNLOADING,   
    ONENET_OTA_EVT_SUCCESS,
    ONENET_OTA_EVT_PLATFORM_ERROR, 
    ONENET_OTA_EVT_FAILED,         
} onenet_ota_event_t;

typedef void (*onenet_ota_event_cb_t)(onenet_ota_event_t evt, int value);

const char* get_app_version(void);

void set_app_valid(int valid);

esp_err_t onenet_ota_upload_version(void);

void onenet_ota_set_event_cb(onenet_ota_event_cb_t cb);

const char* onenet_ota_target_version(void);

bool onenet_ota_is_busy(void);

void onenet_ota_check_async(void);

void onenet_ota_upgrade_async(void);

void onenet_ota_prepare_memory(void);

void onenet_ota_restore_camera(void);

void onenet_ota_start(void);

#endif
