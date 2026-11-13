#ifndef _WEATHER_H_
#define _WEATHER_H_

typedef struct
{
    int high_temp;
    int low_temp;
    char code[4];
}weather_data_pkt_t;

void weather_start(void);

#endif
