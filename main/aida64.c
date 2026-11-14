#include "aida64.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "custom.h"

#define TAG     "AIDA64"

extern gg_ui_t guider_ui;

static void aida64_monitor_task(void* param);

static TaskHandle_t aida64_task_handle = NULL;
static char host_aida64_url[128];
static esp_http_client_handle_t http_handle = NULL;

#define AIDA64_CONNECT_BIT BIT0
static EventGroupHandle_t aida64_event_handle = NULL;
static bool is_http_connect = false;

void aida64_monitor_start(const char* ip)
{
    if(aida64_event_handle == NULL)
        aida64_event_handle = xEventGroupCreate();
    if(aida64_task_handle == NULL)
        xTaskCreatePinnedToCore(aida64_monitor_task,"AIDA64",4096,NULL,3,&aida64_task_handle,1);
    snprintf(host_aida64_url,sizeof(host_aida64_url),"http://%s:%d/sse",ip,
             CONFIG_APP_AIDA64_PORT);
    xEventGroupSetBits(aida64_event_handle,AIDA64_CONNECT_BIT);
}

void aida64_monitor_stop(void)
{
    esp_http_client_close(http_handle);
}

int aida64_monitor_isconnect(void)
{
    return is_http_connect;
}

static bool aida64_monitor_parse(char* data,aida64_data_t* aida64_data)
{
    const char* search_str = NULL;
    if(!data)
        return false;
    //CPU rate
    search_str = strstr(data,"CPU rate");
    if(search_str)
    {
        sscanf(search_str,"%*[^0-9]%d",&aida64_data->cpu_rate);
    }
    else
        return false;
    //CPU temp
    search_str = strstr(data,"CPU temp");
    if(search_str)
    {
        sscanf(search_str,"%*[^0-9]%d",&aida64_data->cpu_temp);
    }
    else
        return false;
    //MEM rate
    search_str = strstr(data,"MEM rate");
    if(search_str)
    {
        sscanf(search_str,"%*[^0-9]%d",&aida64_data->mem_rate);
    }
    else
        return false;
    //MEM use
    search_str = strstr(data,"MEM use");
    if(search_str)
    {
        sscanf(search_str,"%*[^0-9]%d",&aida64_data->mem_use);
    }
    else
        return false;
    return true;
}

static esp_err_t _aida64_http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGW(TAG, "HTTP error");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            is_http_connect = true;
            set_monitor_connect_status(&guider_ui,true);
            break;
        case HTTP_EVENT_ON_DATA: {
            aida64_data_t aida64_data;
            if(aida64_monitor_parse((char*)evt->data,&aida64_data))
            {
                set_monitor_param(&guider_ui,aida64_data.cpu_rate,aida64_data.cpu_temp,aida64_data.mem_rate,aida64_data.mem_use);
            }
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

static void aida64_monitor_task(void* param)
{
    static char default_url[96];
    snprintf(default_url,sizeof(default_url),"http://%s:%d/sse",
             CONFIG_APP_AIDA64_DEFAULT_IP,CONFIG_APP_AIDA64_PORT);
    esp_http_client_config_t config =
    {
        .url = default_url,
        .event_handler = _aida64_http_event_handler,
    };
    http_handle = esp_http_client_init(&config);

    while(1)
    {
        EventBits_t ev = xEventGroupWaitBits(aida64_event_handle,AIDA64_CONNECT_BIT,pdTRUE,pdFAIL,pdMS_TO_TICKS(1000));
        if(ev & AIDA64_CONNECT_BIT)
        {
            esp_http_client_set_url(http_handle,host_aida64_url);
            esp_http_client_set_method(http_handle,HTTP_METHOD_GET);
            esp_http_client_set_header(http_handle,"Accept","text/event-stream");
            ESP_LOGD(TAG,"http perform");
            esp_http_client_perform(http_handle);
            is_http_connect = false;
            set_monitor_connect_status(&guider_ui,false);
        }
    }
}
