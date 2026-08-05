/** ***************************************************************************
 * @file nano_i2c.c
 *
 * @brief nanosic i2c client
 *
 * <em>Copyright (C) 2010, Nanosic, Inc.  All rights reserved.</em>
 * Author : Bin.yuan bin.yuan@nanosic.com
 * */

/*
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
//#include <linux/sys_config.h>

#include "nano_macro.h"
//static bool first = true;
/*i2c slave address用单字节表示*/
static const unsigned int const_iaddr_bytes = 1;
/* I2C internal address max length */
#define INT_ADDR_MAX_BYTES   4

/*store 803`s version code*/
char gVers803x[21]={0};

/*store 176`s version code*/
char gVers176x[21]={0};

short gHallStatus = 0;
/* The immediate and delayed IRQ workers must parse reports in read order. */
static DEFINE_MUTEX(g_nanosic_report_lock);

static bool nanosic_vendor_checksum_valid(const char *packet, size_t packet_len)
{
    const unsigned char *bytes = (const unsigned char *)packet;
    size_t checksum_index;
    unsigned char checksum = 0;
    size_t i;

    if (packet_len < 7)
        return false;

    checksum_index = 6 + bytes[5];
    if (checksum_index >= packet_len)
        return false;

    for (i = 0; i < checksum_index; i++)
        checksum += bytes[i];

    return checksum == bytes[checksum_index];
}
/** **************************************************************************
 * @brief
 *
 ** */
static bool
Nanosic_i2c_specified_packets_detect(char *packet, size_t packet_len)
{
    char *data;
    size_t datalen;
    char source;
    char object;
    char command;
    unsigned char keyboard_status;
    bool connected;
    int ret;

    if (!packet || packet_len < 5)
        return false;

    data = packet + 2;
    datalen = packet_len - 2;
    STREAM_TO_UINT8(source,data);
    STREAM_TO_UINT8(object,data);
    STREAM_TO_UINT8(command,data);

    if(command == 0x1 && source == FIELD_803X &&
       object == FIELD_HOST && datalen >= 25)
    {
        /*803->host 上传803版本号*/
        memcpy(gVers803x, data+2, 20);
        return true;
    }else if(command == 0x1 && source == FIELD_176X &&
             object == FIELD_HOST && datalen >= 6)
    {
        /*keypad->host 上传keypad版本号*/
        short version;
        data++;
        STREAM_TO_UINT16(version,data);
        snprintf(gVers176x,sizeof(gVers176x),"%x",version);
        return true;
    }else if(command == 0xA2 && source == FIELD_176X &&
             object == FIELD_HOST && (unsigned char)packet[1] == 0x31 &&
             packet_len >= 20 && (unsigned char)data[0] >= 13 &&
             data[2] == 0 &&
             nanosic_vendor_checksum_valid(packet, packet_len)) {
        keyboard_status = (unsigned char)data[4];
        gHallStatus = keyboard_status;
        connected = !data[13] && (keyboard_status & 0x63) == 0x23;
        if (connected && !Nanosic_input_enabled()) {
            ret = Nanosic_input_register();
            if (ret)
                dbgprint(ERROR_LEVEL,
                         "Could not register attached keyboard: %d\n", ret);
        } else if (!connected && Nanosic_input_enabled()) {
            Nanosic_input_release();
        }
    }

    return false;
}

/** **************************************************************************
 * @brief 驱动probe时的read接口
 *
 ** */
static int
Nanosic_i2c_read_boot(struct i2c_client* client ,void *buf, size_t len)
{
    unsigned char addr[INT_ADDR_MAX_BYTES];
    struct i2c_msg msg[2];
    int ret;

    if (IS_ERR_OR_NULL(client) || !client->adapter || !buf || !len ||
        len > U16_MAX) {
        dbgprint(ERROR_LEVEL,"invalid boot i2c read arguments\n");
        return -EINVAL;
    }

    addr[0] = client->addr;
    /*
     * Send out the register address...
     */
    msg[0].len   = const_iaddr_bytes;
    msg[0].addr  = client->addr;
    msg[0].flags = 0;
    msg[0].buf   = addr;
	/*
	 * ...then read back the result.
	 */
    msg[1].addr  = client->addr;
    msg[1].flags = I2C_M_RD;
    msg[1].len   = len;
    msg[1].buf   = buf;

    ret = i2c_transfer(client->adapter, msg, 2);
    if (ret != 2) {
        dbgprint(ERROR_LEVEL,"boot i2c read transfer error: %d\n",ret);
        return ret < 0 ? ret : -EIO;
    }

    return (int)len;
}

/** **************************************************************************
 * @brief   驱动probe时的write接口
 *
 ** */
static int
Nanosic_i2c_write_boot(struct i2c_client* client, void *buf, size_t len)
{
    struct i2c_msg msg;
    unsigned char tmp_buf[128]={0};
    int ret;

    if (IS_ERR_OR_NULL(client) || !client->adapter || !buf || !len ||
        len > sizeof(tmp_buf) - const_iaddr_bytes) {
        dbgprint(ERROR_LEVEL,"invalid boot i2c write arguments\n");
        return -EINVAL;
    }

    tmp_buf[0] = client->addr;
    memcpy(tmp_buf + const_iaddr_bytes , buf, len);

    msg.addr = client->addr;
    msg.flags = 0;
    msg.len = len + const_iaddr_bytes;
    msg.buf = tmp_buf;

    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret != 1) {
        dbgprint(ERROR_LEVEL,"boot i2c write transfer error: %d\n",ret);
        return ret < 0 ? ret : -EIO;
    }

    return (int)len;
}

/** **************************************************************************
 * @brief
 *
 ** */
int Nanosic_i2c_read_locked(struct nano_i2c_client *i2c_client, void *buf,
                            size_t len)
{
    struct i2c_adapter *adap;
    unsigned char addr[INT_ADDR_MAX_BYTES];
    struct i2c_msg msg[2];
    int ret;
    int result;

    lockdep_assert_held(&g_nanosic_hw_lock);

    if(IS_ERR_OR_NULL(i2c_client) || !buf || !len || len > U16_MAX){
        dbgprint(ERROR_LEVEL,"invalid i2c read arguments\n");
        return -EINVAL;
    }

    addr[0] = i2c_client->i2c_slave_addr;

    adap = i2c_get_adapter(i2c_client->i2c_bus_id);
    if (!adap) {
        dbgprint(ERROR_LEVEL,"i2c adapter is NULL\n");
        return -ENODEV;
    }
    /*
     * Send out the register address...
     */
    msg[0].len   = const_iaddr_bytes;
    msg[0].addr  = i2c_client->i2c_slave_addr;
    msg[0].flags = 0;
    msg[0].buf   = addr;
    /*
     * ...then read back the result.
     */
    msg[1].addr  = i2c_client->i2c_slave_addr;
    msg[1].flags = I2C_M_RD;
    msg[1].len   = len;
    msg[1].buf   = buf;

    ret = i2c_transfer(adap, msg, 2);
    if (ret == 2)
        result = (int)len;
    else {
        dbgprint(ERROR_LEVEL,"i2c_transfer read error: %d\n", ret);
        result = ret < 0 ? ret : -EIO;
    }

    i2c_put_adapter(adap);

    return result;
}

int Nanosic_i2c_read(struct nano_i2c_client *i2c_client, void *buf,
                     size_t len)
{
    int result;

    if (IS_ERR_OR_NULL(i2c_client) || !buf || !len || len > U16_MAX)
        return -EINVAL;

    mutex_lock(&g_nanosic_hw_lock);
    if (!g_nanosic_hw_ready || i2c_client != gI2c_client)
        result = -ENODEV;
    else if (g_nanosic_hw_recovering)
        result = -EBUSY;
    else if (g_nanosic_hw_suspended) {
        i2c_client->read_pending = true;
        result = -EHOSTDOWN;
    }
    else {
        i2c_client->read_pending = false;
        result = Nanosic_PM_try_wakeup_locked();
        if (!result)
            result = Nanosic_i2c_read_locked(i2c_client, buf, len);
    }
    mutex_unlock(&g_nanosic_hw_lock);

    return result;
}

/** **************************************************************************
 * @brief
 *   write data to 8030x
 ** */
int Nanosic_i2c_write_locked(struct nano_i2c_client *i2c_client, void *buf,
                             size_t len)
{
    struct i2c_msg msg;
    struct i2c_adapter *adap;
    unsigned char tmp_buf[128]={0};
    int ret;
    int result;

    lockdep_assert_held(&g_nanosic_hw_lock);

    if(IS_ERR_OR_NULL(i2c_client) || !buf || !len ||
       len > sizeof(tmp_buf) - const_iaddr_bytes){
        dbgprint(ERROR_LEVEL,"invalid i2c write arguments\n");
        return -EINVAL;
    }

    adap = i2c_get_adapter(i2c_client->i2c_bus_id);
    if (!adap) {
        dbgprint(ERROR_LEVEL,"i2c adapter is NULL\n");
        return -ENODEV;
    }

    tmp_buf[0] = i2c_client->i2c_slave_addr;
    memcpy(tmp_buf + const_iaddr_bytes , buf, len);

    msg.addr = i2c_client->i2c_slave_addr;
    msg.flags = 0;
    msg.len = len + const_iaddr_bytes;
    msg.buf = tmp_buf;

    ret = i2c_transfer(adap, &msg, 1);
    if (ret == 1)
        result = (int)len;
    else {
        dbgprint(ERROR_LEVEL,"i2c_transfer write error: %d\n", ret);
        result = ret < 0 ? ret : -EIO;
    }

    i2c_put_adapter(adap);

    return result;
}

int Nanosic_i2c_write(struct nano_i2c_client *i2c_client, void *buf,
                      size_t len)
{
    int result;

    if (IS_ERR_OR_NULL(i2c_client) || !buf || !len ||
        len > 128 - const_iaddr_bytes)
        return -EINVAL;

    mutex_lock(&g_nanosic_hw_lock);
    if (!g_nanosic_hw_ready || i2c_client != gI2c_client)
        result = -ENODEV;
    else if (g_nanosic_hw_recovering)
        result = -EBUSY;
    else if (g_nanosic_hw_suspended)
        result = -EHOSTDOWN;
    else {
        result = Nanosic_PM_try_wakeup_locked();
        if (!result)
            result = Nanosic_i2c_write_locked(i2c_client, buf, len);
    }
    mutex_unlock(&g_nanosic_hw_lock);

    return result;
}

static int nanosic_i2c_request_status(struct nano_i2c_client *i2c_client,
                                     bool hw_locked)
{
    char status_query[I2C_DATA_LENGTH_WRITE] = {
        0x32, 0x00, 0x4e, 0x31, FIELD_HOST, FIELD_176X,
        0xa1, 0x01, 0x01, 0xda,
    };

    if (hw_locked)
        return Nanosic_i2c_write_locked(i2c_client, status_query,
                                       sizeof(status_query));

    return Nanosic_i2c_write(i2c_client, status_query, sizeof(status_query));
}

int Nanosic_i2c_request_status(struct nano_i2c_client *i2c_client)
{
    return nanosic_i2c_request_status(i2c_client, false);
}

int Nanosic_i2c_request_status_locked(struct nano_i2c_client *i2c_client)
{
    lockdep_assert_held(&g_nanosic_hw_lock);
    return nanosic_i2c_request_status(i2c_client, true);
}

/** **************************************************************************
 * @brief   i2c上传数据的解析 , 数据格式参见 <IIC总线协议>文挡
 *
 ** */

int
Nanosic_i2c_parse(char* data, size_t datalen)
{
    char* p = data;
    size_t left;
    unsigned char  first_byte;  /*fixed data 0x57 or 0xD7*/
    unsigned char  third_byte;  /*fixed data 0x39 , 0x4A , 0x5B , 0x6C*/
    unsigned char  fourth_byte; /*fixed data 0x2: mouse  0x5: keyboard 0x19: touch*/

    if (!data || datalen < 3) {
        dbgprint(ERROR_LEVEL , "invalid data length\n");
        return -EINVAL;
    }

    left = datalen;

//    atomic_inc(&i2c_received);

    STREAM_TO_UINT8(first_byte,p);
    left--;
    if(first_byte != 0x57 && first_byte != 0xD7){
        dbgprint(DEBUG_LEVEL , "first_byte error\n");
        return -1;
    }

    p++;
    left--;

    STREAM_TO_UINT8(third_byte,p);
    left--;

    if(third_byte == 0){ /*null packet*/
        //dbgprint(DEBUG_LEVEL , "third_byte is null\n");
        return -1;
    }

    if(third_byte != 0x39 && third_byte != 0x4A && third_byte != 0x5B && third_byte != 0x6C){
        /*no packets include*/
        dbgprint(DEBUG_LEVEL , "third_byte error\n");
        return -1;
    }

    rawdata_show("_rawdata_",data,datalen);

    while(left > 0)
    {
        if (left < 3)
            return -EPROTO;

        fourth_byte = p[0];

        switch(fourth_byte)
        {
            case 0x5:
                if (left < 9)
                    return -EPROTO;
                /*05 00 00 52 00 00 00 00 00 上*/
                /*05 00 00 51 00 00 00 00 00 下*/
                if(g_panel_status){
                    Nanosic_cache_put();
                    Nanosic_input_write(EM_PACKET_KEYBOARD,p,9);
                    p += 9;
                    left -= 9;
                }else{
                    Nanosic_cache_insert(EM_PACKET_KEYBOARD,p,9);
                    p += 9;
                    left -= 9;
                }
                break;
            case 0x6:
                if (left < 5)
                    return -EPROTO;
                if(g_panel_status){
                    Nanosic_cache_put();
                    Nanosic_input_write(EM_PACKET_CONSUMER,p,5);
                    p += 5;
                    left -= 5;
                }else{
                    Nanosic_cache_insert(EM_PACKET_CONSUMER,p,5);
                    p += 5;
                    left -= 5;
                }
                break;
            case 0x2:
                if (left < 8)
                    return -EPROTO;
                Nanosic_input_write(EM_PACKET_MOUSE,p,8);
                p += 8;
                left -= 8;
                break;
            case 0x19:
                if (left < 21)
                    return -EPROTO;
                Nanosic_input_write(EM_PACKET_TOUCH,p,21);
                p += 21;
                left -= 21;
                break;
            case 0x22:
                if (left < 16)
                    return -EPROTO;
                Nanosic_i2c_specified_packets_detect(p, 16);
                Nanosic_chardev_client_write(p,16);     /* upstreams data via /dev/nanodev0 to userspace*/
                p += 16;
                left -= 16;
                break;
            case 0x23:
                if (left < 32)
                    return -EPROTO;
                Nanosic_i2c_specified_packets_detect(p, 32);
                Nanosic_chardev_client_write(p,32);
                Nanosic_input_write(EM_PACKET_VENDOR,p,32);
                p += 32;
                left -= 32;
                break;
            case 0x26:
            case 0x24:
            //case 0x23:
            {
                if (left >= 5)
                    Nanosic_i2c_specified_packets_detect(p, left);
                Nanosic_chardev_client_write(p,left);  /* upstreams data via /dev/nanodev0 to userspace*/
                return 0;
            }

            default:
                left = 0;
                break;
        }
    }

    return 0;
}

/** ************************************************************************//**
 * @brief   Nanosic_i2c_read_process
 *          工作队列下半部读取i2c数据并解析
 ** */
int
Nanosic_i2c_read_handler(void* data)
{
    struct nano_i2c_client* i2c_client = (struct nano_i2c_client*) data;

    char  buf[I2C_DATA_LENGTH_READ]={0};
    int   datalen;

    if(IS_ERR_OR_NULL(i2c_client))
    {
        dbgprint(ALERT_LEVEL,"Nanosic_i2c_read_handler, null\n");
        return -1;
    }

    mutex_lock(&g_nanosic_report_lock);
    datalen = Nanosic_i2c_read(i2c_client,buf,sizeof(buf));
    if(datalen > 0){
        Nanosic_i2c_parse(buf,datalen);
    }else{
        atomic_inc(&i2c_client->i2c_error_count);
    }
    atomic_inc(&i2c_client->i2c_read_count);
    mutex_unlock(&g_nanosic_report_lock);

    return 0;
}
#ifdef I2C_GPIO_IRQNO
/** ************************************************************************//**
 * @brief   Nanosic_i2c_irq
 *          I2C GPIO 中断处理程序
 ** */
irqreturn_t
Nanosic_i2c_irq(int irq, void *dev_id)
{
    struct nano_i2c_client* i2c_client = (struct nano_i2c_client*)dev_id;

    if(IS_ERR_OR_NULL(i2c_client)){
        dbgprint(ERROR_LEVEL,"i2c_client is NULL\n");
        return IRQ_HANDLED;
    }

    Nanosic_workQueue_schedule(i2c_client->worker);

    return IRQ_HANDLED;
}
#endif

int
Nanosic_i2c_read_version(struct i2c_client *client)
{
    char rsp[I2C_DATA_LENGTH_READ]={0};
    char cmd[I2C_DATA_LENGTH_WRITE]={0x32,0x00,0x4F,0x30,0x80,0x18,0x01,0x00,0x18};
    u8  retry = 0;
    int ret = -1;

    while(retry++ < 30)
    {
        ret = Nanosic_i2c_write_boot(client,cmd,sizeof(cmd));
        if(ret != (int)sizeof(cmd)){
            dbgprint(ALERT_LEVEL,"i2c write cmd failed time %d\n",retry);
            msleep(100);
            continue;
        }
        dbgprint(ALERT_LEVEL,"i2c write OK %d\n",ret);
        msleep(2);
        ret = Nanosic_i2c_read_boot(client,rsp,sizeof(rsp));
        dbgprint(ALERT_LEVEL,"i2c read %d\n",ret);
        if(ret == (int)sizeof(rsp)){
            rawdata_show("recv vers",rsp,sizeof(rsp));
            break;
        }
        msleep(2);
    }

    return ret == (int)sizeof(rsp) ? 0 : (ret < 0 ? ret : -EIO);
}

int
Nanosic_i2c_detect(struct i2c_client *client, struct i2c_board_info *info)
{
    struct i2c_adapter *adapter = client->adapter;
    int  ret = -1;

    if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)){
        dbgprint(ALERT_LEVEL,"======return=====\n");
        return -ENODEV;
    }

    dbgprint(ALERT_LEVEL,"%s: addr = %x\n", __func__, client->addr);

    ret = Nanosic_i2c_read_version(client);
    if (ret)
        return ret;

    strscpy(info->type, "nanosic,803", I2C_NAME_SIZE);
    return 0;
}

/** ************************************************************************//**
 *  @func Nanosic_i2c_register
 *
 *  @brief 注册gpio下降沿中断 , 当有中断触发时去获取i2c数据
 *
 ** */
struct nano_i2c_client*
Nanosic_i2c_register(int irqno,u32 irq_flags,int i2c_bus_id, int i2c_slave_addr)
{
    struct gpio_desc *irq_desc;
    int ret;
    struct nano_i2c_client* i2c_client = kzalloc(sizeof(struct nano_i2c_client),GFP_KERNEL);
    if (!i2c_client) {
        dbgprint(ERROR_LEVEL,"Could not alloc memory\n");
        return ERR_PTR(-ENOMEM);
    }

    if (i2c_bus_id < 0 || i2c_slave_addr < 0 || i2c_slave_addr > 0x7f ||
        !gpio_is_valid(irqno)) {
        dbgprint(ERROR_LEVEL,"Invalid argments\n");
        ret = -EINVAL;
        goto _err1;
    }

    irq_desc = gpio_to_desc(irqno);
    if (!irq_desc) {
        ret = -ENODEV;
        goto _err1;
    }

    dbgprint(DEBUG_LEVEL,"new i2c %p\n",i2c_client);

    atomic_set(&i2c_client->i2c_read_count, 0);
    atomic_set(&i2c_client->i2c_error_count, 0);
    i2c_client->i2c_bus_id     = i2c_bus_id;
    i2c_client->i2c_slave_addr = i2c_slave_addr;
    i2c_client->irqno = gpiod_to_irq(irq_desc);
    if (i2c_client->irqno < 0) {
        ret = i2c_client->irqno;
        goto _err1;
    }
    i2c_client->irqflags = irq_flags;
    i2c_client->func = Nanosic_i2c_read_handler;
    /*initialize wakeup_irq spinlock*/
    spin_lock_init(&i2c_client->input_report_lock);
    i2c_client->input_dev = input_allocate_device();
    if (!i2c_client->input_dev) {
        dbgprint(ERROR_LEVEL, "No such input device defined! \n");
        ret = -ENOMEM;
        goto _err1;
    }

    __set_bit(EV_SYN, i2c_client->input_dev->evbit);
    __set_bit(EV_KEY, i2c_client->input_dev->evbit);
    i2c_client->input_dev->name = "xiaomi keyboard WakeUp";
    i2c_client->input_dev->id.product = 0x94;
    i2c_client->input_dev->id.vendor = 0x0827;

    input_set_capability(i2c_client->input_dev, EV_KEY, KEY_WAKEUP);
    ret = input_register_device(i2c_client->input_dev);
    if (ret) {
        dbgprint(ERROR_LEVEL, "No such input device\n");
        goto _err_input_free;
    }

    /*initialize workqueue*/
    i2c_client->worker = Nanosic_workQueue_register(i2c_client);
    if(IS_ERR_OR_NULL(i2c_client->worker)){
        dbgprint(ERROR_LEVEL,"Could not register workqueue\n");
        ret = -ENOMEM;
        goto _err_input_unregister;
    }

    dbgprint(DEBUG_LEVEL,"new worker %p\n",i2c_client->worker);

#ifdef I2C_GPIO_IRQNO
    dbgprint(INFO_LEVEL,"irq-gpio:%d irq-no:%d irq_flags:%d \n",irqno,i2c_client->irqno,irq_flags);
    ret = request_threaded_irq(i2c_client->irqno, NULL, Nanosic_i2c_irq,
                               irq_flags, "8030_io_irq", i2c_client);
    if (ret) {
        dbgprint(ERROR_LEVEL,"Could not register for %s interrupt, irq = %d, ret = %d\n","8030_io_irq", i2c_client->irqno, ret);
        goto _err_worker;
    }
    i2c_client->irq_registered = true;
#endif
    return i2c_client;

#ifdef I2C_GPIO_IRQNO
_err_worker:
    Nanosic_workQueue_release(i2c_client->worker);
#endif
_err_input_unregister:
    input_unregister_device(i2c_client->input_dev);
    i2c_client->input_dev = NULL;
    goto _err1;
_err_input_free:
    input_free_device(i2c_client->input_dev);
    i2c_client->input_dev = NULL;
_err1:
    kfree(i2c_client);

    return ERR_PTR(ret);
}

/** ************************************************************************//**
 *  @func Nanosic_i2c_release
 *
 *  @brief
 *
 ** */
void
Nanosic_i2c_release(struct nano_i2c_client* i2c_client)
{
    if(IS_ERR_OR_NULL(i2c_client))
        return;

#ifdef I2C_GPIO_IRQNO
    mutex_lock(&g_nanosic_hw_lock);
    if(i2c_client->irq_registered) {
        free_irq(i2c_client->irqno , i2c_client);
        i2c_client->irq_registered = false;
    }
    mutex_unlock(&g_nanosic_hw_lock);
#endif
    Nanosic_workQueue_release(i2c_client->worker);
    if (i2c_client->input_dev) {
        input_unregister_device(i2c_client->input_dev);
        i2c_client->input_dev = NULL;
    }
    kfree(i2c_client);
}
