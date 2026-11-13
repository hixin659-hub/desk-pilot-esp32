#ifndef _XL9555_H_
#define _XL9555_H_

#include "esp_err.h"
#include "driver/gpio.h"

//port 0
#define IO0_0  (1<<0)
#define IO0_1  (1<<1)
#define IO0_2  (1<<2)
#define IO0_3  (1<<3)
#define IO0_4  (1<<4)
#define IO0_5  (1<<5)
#define IO0_6  (1<<6)
#define IO0_7  (1<<7)
//port 1
#define IO1_0  (1<<8)
#define IO1_1  (1<<9)
#define IO1_2 (1<<10)
#define IO1_3 (1<<11)
#define IO1_4 (1<<12)
#define IO1_5 (1<<13)
#define IO1_6 (1<<14)
#define IO1_7 (1<<15)

typedef void (*xl9555_input_callback_t)(uint16_t pin, int level);

// 初始化XL9555
esp_err_t xl9555_init(gpio_num_t scl, gpio_num_t sda, gpio_num_t isr, xl9555_input_callback_t f);

//向xl9555写入2字节数据
esp_err_t xl9555_write_word(uint8_t reg, uint16_t data);

//从xl9555读取2字节数据
esp_err_t xl9555_read_word(uint8_t reg, uint16_t *data);

esp_err_t xl9555_ioconfig(uint16_t config);

esp_err_t xl9555_pin_write(uint16_t pin, int level);

#endif
