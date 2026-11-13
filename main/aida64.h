#ifndef _AIDA64_H_
#define _AIDA64_H_

typedef struct 
{
    int cpu_rate;
    int cpu_temp;
    int mem_rate;
    int mem_use;
}aida64_data_t;


void aida64_monitor_start(const char* ip);

void aida64_monitor_stop(void);

int aida64_monitor_isconnect(void);

#endif
