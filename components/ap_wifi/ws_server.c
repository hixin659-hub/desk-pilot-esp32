#include "ws_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "string.h"

#define TAG "ws_server"

//网页
static const char *http_html = NULL;

//websocket 接收数据回调函数
static ws_receive_cb ws_receive_fn = NULL;

static int client_fds = -1;

static httpd_handle_t server_handle;

static esp_err_t handle_ws_req(httpd_req_t *r)
{
    //IDF v6.x: the server completes the WS handshake itself and never calls
    //the handler with HTTP_GET, so the fd must be captured on every frame
    //(the HTTP_GET branch below is kept only for older IDF versions)
    client_fds = httpd_req_to_sockfd(r);
    if(r->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "WebSocket client connected");
        return ESP_OK;
    }
    httpd_ws_frame_t pkt;
    uint8_t *buf = NULL;
    memset(&pkt,0,sizeof(httpd_ws_frame_t)); 
    esp_err_t ret = httpd_ws_recv_frame(r, &pkt, 0);
    if(ret != ESP_OK)
    {
        return ret;
    }
    if(pkt.len)
    {
       buf = calloc(1, pkt.len + 1);
        if (buf == NULL)
        {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        pkt.payload = buf;
        ret = httpd_ws_recv_frame(r, &pkt, pkt.len);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
    }

    if (pkt.type == HTTPD_WS_TYPE_TEXT && ws_receive_fn)
    {
        ws_receive_fn(pkt.payload,pkt.len);
    }
    free(buf);
    return ESP_OK;
}

esp_err_t get_http_req(httpd_req_t *r)
{
    esp_err_t response = ESP_FAIL;
    if(http_html)
    {
        response = httpd_resp_send(r, http_html, HTTPD_RESP_USE_STRLEN);
    }
    return response;
}

esp_err_t wb_ws_send(uint8_t *data, int len)
{
    if (httpd_ws_get_fd_info(server_handle, client_fds) != HTTPD_WS_CLIENT_WEBSOCKET)
    {
        ESP_LOGW(TAG, "WS client not connected, drop send");
        return ESP_FAIL;
    }
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = data;
    pkt.len = len;
    pkt.type = HTTPD_WS_TYPE_TEXT;
    return httpd_ws_send_data(server_handle, client_fds, &pkt);
}

esp_err_t wb_ws_start(ws_cfg_t *cfg)
{
    if(cfg == NULL )
      return ESP_FAIL;
    http_html = cfg->html_code;
    ws_receive_fn = cfg->receive_fn;
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    httpd_uri_t uri_get =
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_http_req,
    };

    httpd_uri_t uri_ws =
    {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .is_websocket = true,
    };
      if (httpd_start(&server_handle, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server_handle, &uri_get);
        httpd_register_uri_handler(server_handle, &uri_ws);
    }
    return ESP_OK;
}

esp_err_t wb_ws_stop(void)
{
    if(server_handle)
    {
        httpd_stop(server_handle);
        server_handle = NULL;
    }
    return ESP_OK;
}
