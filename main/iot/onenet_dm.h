#ifndef _ONENET_DM_H_
#define _ONENET_DM_H_
#include "cJSON.h"

void onenet_dm_init(void);

void onenet_property_handle(cJSON* property);

cJSON* onenet_property_upload(void);

#endif
