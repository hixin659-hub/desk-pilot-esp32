#ifndef _ONENET_MQTT_H_
#define _ONENET_MQTT_H_
#include "esp_err.h"
#include "onenet_cfg.h"

esp_err_t onenet_start(void);

esp_err_t onenet_post_property_data(const char* data);

#endif
