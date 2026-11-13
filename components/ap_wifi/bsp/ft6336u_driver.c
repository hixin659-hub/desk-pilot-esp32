#include "ft6336u_driver.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#define TAG  "ft6336u"

static i2c_master_bus_handle_t  ft6336u_bus_handle = NULL;
static i2c_master_dev_handle_t  ft6336u_dev_handle = NULL;
static bool ft6336u_int_flag = false;

esp_err_t ft6336u_driver(gpio_num_t sda,gpio_num_t scl)
{
    //i2c总线初始化
    i2c_master_bus_config_t bus_config = 
    {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .glitch_ignore_cnt = 7,
        .i2c_port = -1,
    };
    i2c_new_master_bus(&bus_config,&ft6336u_bus_handle);

    //添加i2c设备
    i2c_device_config_t dev_config = 
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x38,
        .scl_speed_hz = 400*1000,
    };
    i2c_master_bus_add_device(ft6336u_bus_handle,&dev_config,&ft6336u_dev_handle);

    uint8_t read_buff[1];
    uint8_t write_buff[2];
    esp_err_t ret = ESP_FAIL;
    write_buff[0] = 0xA3;
    ret = i2c_master_transmit_receive(ft6336u_dev_handle,&write_buff[0],1,&read_buff[0],1,1000);
    if(ret != ESP_OK)
    {
        return ret;
    }
    write_buff[0] = 0xA4;
    write_buff[1] = 0x00;
    ret = i2c_master_transmit(ft6336u_dev_handle,&write_buff[0],2,1000);

    return ret;
}

void ft6336u_read(int16_t* x,int16_t* y,int* state)
{
    uint8_t read_buff[5];
    uint8_t write_buff[1];
    static int16_t last_x = 0;
    static int16_t last_y = 0;

    if(!ft6336u_int_flag)
    {
        *x = last_x;
        *y = last_y;
        *state = 0;
        return;
    }
    write_buff[0] = 0x02;
    i2c_master_transmit_receive(ft6336u_dev_handle,&write_buff[0],1,&read_buff[0],5,500);
    if((read_buff[0] & 0x0f) != 1)
    {
        *x = last_x;
        *y = last_y;
        *state = 0;
        return;
    }
    int16_t curret_x = ((read_buff[1]&0x0F)<<8 | read_buff[2]);
    int16_t curret_y = ((read_buff[3]&0x0F)<<8 | read_buff[4]);
    last_x = curret_x;
    last_y = curret_y;
    *x = curret_x;
    *y = curret_y;
    *state = 1;
}

void ft6336u_int_info(bool flag)
{
    ft6336u_int_flag = flag;
}
