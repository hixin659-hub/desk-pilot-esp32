#include "onenet_ota.h"
#include "onenet_cfg.h"
#include "esp_ota_ops.h"
#include <stdio.h>
#include <stdint.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "onenet_token.h"
#include "string.h"
#include "cJSON.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include <stdlib.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "usb_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG  "onenet_ota"

#define TOKEN_VALID_TIMESTAMP 1917014400

#define ONENET_OTA_URL       "https://iot-api.heclouds.com/fuse-ota"
#define ONENET_OTA_HOST      "iot-api.heclouds.com"

static char target_version[32] = {0};
static int task_id = 0;

static bool ota_is_running = false;
static onenet_ota_event_cb_t s_event_cb = NULL;

static void ota_emit(onenet_ota_event_t evt, int value)
{
    onenet_ota_event_cb_t cb = s_event_cb;
    if(cb != NULL)
    {
        cb(evt, value);
    }
}

static void ota_log_heap(const char* stage)
{
    ESP_LOGI(TAG, "%s: internal free=%u largest=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

/* OTA 前挂起 UVC，释放 URB 占用的内部 RAM（约 30KB） */
void onenet_ota_prepare_memory(void)
{
    usb_camera_set_suspended(true);
    for(int i = 0; i < 30 && !usb_camera_is_closed(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(!usb_camera_is_closed())
    {
        ESP_LOGW(TAG, "UVC close timeout");
    }
    ota_log_heap("memory prepared");
}

void onenet_ota_restore_camera(void)
{
    usb_camera_set_suspended(false);
}

const char* get_app_version(void)
{
    static char app_version[32] = {0};
    if(app_version[0] == 0)
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_app_desc_t app_desc;
        memset(&app_desc,0,sizeof(app_desc));
        if(esp_ota_get_partition_description(running,&app_desc) == ESP_OK)
        {
            snprintf(app_version,sizeof(app_version),"%s",app_desc.version);
        }
        else
        {
            snprintf(app_version,sizeof(app_version),"%s","unknown");
        }
    }
    return app_version;
}

void set_app_valid(int valid)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if(esp_ota_get_state_partition(running,&state) == ESP_OK)
    {
        if(state == ESP_OTA_IMG_PENDING_VERIFY)
        {
            if(valid)
            {
                esp_ota_mark_app_valid_cancel_rollback();
            }
            else
            {
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }
}

#define OTA_BUFF_LEN 1024
static uint8_t ota_data_buff[OTA_BUFF_LEN];
static int ota_data_size = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_HEADERS_COMPLETE:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            printf("HTTP_EVENT_ON_DATA data=%.*s\r\n",evt->data_len,(char*)evt->data);
            int copy_len = 0;
            if(evt->data_len > OTA_BUFF_LEN - ota_data_size)
            {
                copy_len = OTA_BUFF_LEN - ota_data_size;
            }
            else
            {
                copy_len = evt->data_len;
            }
            if(copy_len > 0)
            {
                memcpy(&ota_data_buff[ota_data_size],evt->data,(size_t)copy_len);
                ota_data_size += copy_len;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t onenet_ota_http_connect(const char* url,esp_http_client_method_t method,const char* payload)
{
    esp_http_client_config_t config =
    {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(client == NULL)
    {
        ESP_LOGE(TAG,"esp_http_client_init failed");
        return ESP_FAIL;
    }
    char* token = (char*)malloc(256);
    if(token == NULL)
    {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    memset(token,0,256);
    if(dev_token_generate(token,SIG_METHOD_SHA256, TOKEN_VALID_TIMESTAMP, ONENET_PRODUCT_ID, NULL, ONENET_PRODUCT_ACCESS_KEY) != 0)
    {
        ESP_LOGE(TAG,"dev_token_generate failed");
        free(token);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ota token:%s",token);
    // POST
    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "host", ONENET_OTA_HOST);
    esp_http_client_set_header(client, "Authorization", token);
    if(payload)
    {
        ESP_LOGI(TAG,"post data:%s",payload);
        esp_http_client_set_post_field(client, payload, strlen(payload));
    }
    memset(ota_data_buff,0,sizeof(ota_data_buff));
    ota_data_size = 0;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if(err == ESP_OK && status != 200)
    {
        ESP_LOGW(TAG, "HTTP status=%d", status);
    }
    free(token);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t onenet_ota_upload_version(void)
{
    char url[160];
    char version[160];
    const char* app_version = get_app_version();
    esp_err_t ret = ESP_FAIL;
    snprintf(url,sizeof(url),ONENET_OTA_URL"/%s/%s/version",ONENET_PRODUCT_ID,ONENET_DEVICE_NAME);
    snprintf(version,sizeof(version),"{\"s_version\":\"%s\", \"f_version\": \"%s\"}",app_version,app_version);
    if(ESP_OK == onenet_ota_http_connect(url,HTTP_METHOD_POST,version))
    {
        cJSON* root = cJSON_ParseWithLength((const char*)ota_data_buff,(size_t)ota_data_size);
        if(root)
        {
            cJSON* code_js = cJSON_GetObjectItem(root,"code");
            if(code_js && cJSON_GetNumberValue(code_js) == 0)
                ret = ESP_OK;
            cJSON_Delete(root);
        }
    }
    if(ret != ESP_OK)
    {
        ESP_LOGI(TAG,"Upload version failed");
    }
    return ret;
}

typedef enum {
    OTA_QUERY_UPDATE = 0,
    OTA_QUERY_NONE,
    OTA_QUERY_ERROR,         
    OTA_QUERY_PLATFORM_ERROR,
} ota_query_result_t;

static int s_platform_code = 0;

static ota_query_result_t onenet_ota_query_task(const char* type, const char* version)
{
    char url[192];
    ota_query_result_t result = OTA_QUERY_ERROR;
    snprintf(url,sizeof(url),ONENET_OTA_URL"/%s/%s/check?type=%s&version=%s",
             ONENET_PRODUCT_ID,ONENET_DEVICE_NAME,type,version);
    if(ESP_OK != onenet_ota_http_connect(url,HTTP_METHOD_GET,NULL))
    {
        return OTA_QUERY_ERROR;
    }

    cJSON* root = cJSON_ParseWithLength((const char*)ota_data_buff,(size_t)ota_data_size);
    if(root == NULL)
    {
        return OTA_QUERY_ERROR;
    }

    cJSON* code_js = cJSON_GetObjectItem(root,"code");
    cJSON* data_js = cJSON_GetObjectItem(root,"data");
    if(code_js && cJSON_GetNumberValue(code_js) == 0)
    {
        result = OTA_QUERY_NONE;

        cJSON* target_js = cJSON_GetObjectItem(data_js,"target");
        cJSON* tid_js = cJSON_GetObjectItem(data_js,"tid");
        if(tid_js == NULL && target_js)
        {
            tid_js = cJSON_GetObjectItem(target_js,"tid");
        }
        const char* target_ver = cJSON_GetStringValue(target_js);
        if(target_ver == NULL && target_js)
        {
            target_ver = cJSON_GetStringValue(cJSON_GetObjectItem(target_js,"version"));
        }

        if(target_ver && tid_js)
        {
            snprintf(target_version,sizeof(target_version),"%s",target_ver);
            task_id = (int)cJSON_GetNumberValue(tid_js);
            result = OTA_QUERY_UPDATE;
        }
        else
        {
            ESP_LOGI(TAG, "no ota task");
        }
    }
    else
    {
        s_platform_code = code_js ? (int)cJSON_GetNumberValue(code_js) : -1;
        result = OTA_QUERY_PLATFORM_ERROR;
        ESP_LOGW(TAG, "check ota task platform code=%d", s_platform_code);
    }
    cJSON_Delete(root);
    return result;
}

esp_err_t onenet_ota_check_status(int tid,int step)
{
    char url[160];
    char payload[16];
    esp_err_t ret = ESP_FAIL;
    snprintf(url,sizeof(url),ONENET_OTA_URL"/%s/%s/%d/status",ONENET_PRODUCT_ID,ONENET_DEVICE_NAME,tid);
    snprintf(payload,sizeof(payload),"{\"step\":%d}",step);
    if(ESP_OK == onenet_ota_http_connect(url,HTTP_METHOD_POST,payload))
    {
        cJSON* root = cJSON_ParseWithLength((const char*)ota_data_buff,(size_t)ota_data_size);
        if(root)
        {
            cJSON* code_js = cJSON_GetObjectItem(root,"code");
            if(code_js && cJSON_GetNumberValue(code_js) == 0)
                ret = ESP_OK;
            cJSON_Delete(root);
        }
    }
    if(ret != ESP_OK)
    {
        ESP_LOGI(TAG,"Check status failed");
    }
    return ret;
}

esp_err_t onenet_ota_init_cb(esp_http_client_handle_t client)
{
    static char token[256];
    memset(token,0,256);
    if(dev_token_generate(token,SIG_METHOD_SHA256, TOKEN_VALID_TIMESTAMP, ONENET_PRODUCT_ID, NULL, ONENET_PRODUCT_ACCESS_KEY) != 0)
    {
        ESP_LOGE(TAG,"download token generate failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "download token:%s", token);
    // GET
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "host", ONENET_OTA_HOST);
    esp_http_client_set_header(client, "Authorization", token);
    return ESP_OK;
}

static int64_t s_dl_total = -1;
static int64_t s_dl_done = 0;
static int s_dl_pct = -1;

static esp_err_t onenet_ota_download_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id)
    {
    case HTTP_EVENT_ON_CONNECTED:
        s_dl_total = -1;
        s_dl_done = 0;
        s_dl_pct = -1;
        break;
    case HTTP_EVENT_ON_DATA:
    {
        int64_t len = esp_http_client_get_content_length(evt->client);
        if(len >= 4096)
        {
            if(len != s_dl_total)
            {
                s_dl_total = len;
                s_dl_done = 0;
                s_dl_pct = -1;
            }
            s_dl_done += evt->data_len;
            int pct = (int)((s_dl_done * 100) / s_dl_total);
            if(pct > 100)
                pct = 100;
            if(pct != s_dl_pct)
            {
                s_dl_pct = pct;
                ota_emit(ONENET_OTA_EVT_DOWNLOADING, pct);
            }
        }
        else if(len >= 0)
        {
            ESP_LOGW(TAG,"DL unexpected small body(%lld): %.*s",(long long)len,(int)evt->data_len,(char*)evt->data);
        }
        break;
    }
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG,"DL HTTP_EVENT_ERROR");
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t onenet_ota_download(int tid)
{
    char url[256];
    snprintf(url,sizeof(url),ONENET_OTA_URL"/%s/%s/%d/download",ONENET_PRODUCT_ID,ONENET_DEVICE_NAME,tid);
    ESP_LOGI(TAG,"download url:%s",url);
    esp_http_client_config_t http_cfg =
    {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = onenet_ota_download_event_handler,
    };
    esp_https_ota_config_t ota_cfg =
    {
        .http_config = &http_cfg,
        .http_client_init_cb = onenet_ota_init_cb,
    };
    esp_err_t ota_ret = esp_https_ota(&ota_cfg);
    if(ota_ret == ESP_OK)
    {
        ESP_LOGI(TAG,"Upgrade successful");
    }
    else
    {
        ESP_LOGI(TAG,"Upgrade failed code:%d",ota_ret);
    }
    return ota_ret;
}

typedef enum {
    OTA_ACTION_CHECK = 0,
    OTA_ACTION_UPGRADE,
    OTA_ACTION_FULL,
} ota_action_t;

static void onenet_ota_task(void* param)
{
    ota_action_t action = (ota_action_t)(intptr_t)param;
    esp_err_t ret = ESP_OK;

    ota_log_heap("ota task running");

    if(action == OTA_ACTION_CHECK || action == OTA_ACTION_FULL)
    {
        //上报版本号
        ota_emit(ONENET_OTA_EVT_CHECKING, 0);
        ret = onenet_ota_upload_version();
        if(ret != ESP_OK)
        {
            ESP_LOGI(TAG,"Upload_version fail");
            goto finished;
        }
        ota_emit(ONENET_OTA_EVT_VERSION_UPLOADED, 0);

        //检测升级任务
        ota_query_result_t q = onenet_ota_query_task("1",get_app_version());
        if(q == OTA_QUERY_ERROR)
        {
            ESP_LOGI(TAG,"Check_task fail");
            ret = ESP_FAIL;
            goto finished;
        }
        if(q == OTA_QUERY_PLATFORM_ERROR)
        {
            ota_is_running = false;
            ota_emit(ONENET_OTA_EVT_PLATFORM_ERROR, s_platform_code);
            goto done;
        }
        if(q == OTA_QUERY_NONE)
        {
            ota_is_running = false;
            ota_emit(ONENET_OTA_EVT_NO_UPDATE, 0);
            goto done;
        }
        if(action == OTA_ACTION_CHECK)
        {
            /* 先清 busy 再通知，避免用户立刻点“立即升级”时被当成正在运行 */
            ota_is_running = false;
            ota_emit(ONENET_OTA_EVT_UPDATE_FOUND, 0);
            goto done;
        }
        ota_emit(ONENET_OTA_EVT_UPDATE_FOUND, 0);
    }

    if(target_version[0] == 0 || task_id <= 0)
    {
        ret = ESP_ERR_INVALID_STATE;
        goto finished;
    }

    //开始下载
    ota_emit(ONENET_OTA_EVT_DOWNLOADING, -1);

    //上报任务状态 10%
    ret = onenet_ota_check_status(task_id,10);
    if(ret != ESP_OK)
    {
        ESP_LOGI(TAG,"Check_status fail");
        goto finished;
    }

    //进行HTTP下载
    ret = onenet_ota_download(task_id);
    if(ret != ESP_OK)
    {
        ESP_LOGI(TAG,"Download fail");
        goto finished;
    }

    //上报任务升级状态 100%
    ret = onenet_ota_check_status(task_id,100);
    if(ret != ESP_OK)
    {
        ESP_LOGI(TAG,"Check_status fail");
        goto finished;
    }

    ota_emit(ONENET_OTA_EVT_SUCCESS, 100);

    //留一点时间给 UI 显示结果
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

finished:
    ota_is_running = false;
    onenet_ota_restore_camera();
    ota_emit(ONENET_OTA_EVT_FAILED, (int)ret);
done:
    onenet_ota_restore_camera();
    vTaskDelete(NULL);
}

static void ota_start_task(ota_action_t action)
{
    if(ota_is_running)
    {
        return;
    }
    /* 先释放相机占用的内部 RAM */
    onenet_ota_prepare_memory();

    ota_log_heap("ota start");
    ota_is_running = true;
    if(xTaskCreatePinnedToCore(onenet_ota_task,"onenet_ota",8192,(void*)(intptr_t)action,2,NULL,1) != pdPASS)
    {
        ota_is_running = false;
        onenet_ota_restore_camera();
        ESP_LOGE(TAG, "xTaskCreate failed");
        ota_log_heap("task create failed");
        ota_emit(ONENET_OTA_EVT_FAILED, (int)ESP_ERR_NO_MEM);
    }
}

void onenet_ota_set_event_cb(onenet_ota_event_cb_t cb)
{
    s_event_cb = cb;
}

const char* onenet_ota_target_version(void)
{
    return target_version;
}

bool onenet_ota_is_busy(void)
{
    return ota_is_running;
}

void onenet_ota_check_async(void)
{
    ota_start_task(OTA_ACTION_CHECK);
}

void onenet_ota_upgrade_async(void)
{
    ota_start_task(OTA_ACTION_UPGRADE);
}

void onenet_ota_start(void)
{
    ota_start_task(OTA_ACTION_FULL);
}
