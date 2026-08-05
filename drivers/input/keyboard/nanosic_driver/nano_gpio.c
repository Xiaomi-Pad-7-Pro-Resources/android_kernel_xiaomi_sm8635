/** ***************************************************************************
 * @file nano_gpio.c
 *
 * @brief nanosic gpio file
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
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include "nano_macro.h"
#include <linux/interrupt.h>

static bool registered  = false;
static int gpio_reset_pin = -1;
static int gpio_status_pin = -1;
static int gpio_vdd_pin = -1;
static int gpio_irq_pin = -1;
static int gpio_sleep_pin = -1;
static struct gpio_desc *gpio_reset_desc;
static struct gpio_desc *gpio_status_desc;
static struct gpio_desc *gpio_vdd_desc;
static struct gpio_desc *gpio_irq_desc;
static struct gpio_desc *gpio_sleep_desc;
static int sleep_pin_status = -1;
int g_wakeup_irqno  = -1;
static bool wakeup_irq_registered;
int gpio_hall_n_pin = -1;
int gpio_hall_s_pin = -1;

bool g_panel_status = false;/*1:on, 0:off*/
DEFINE_MUTEX(g_nanosic_lifecycle_lock);
DEFINE_MUTEX(g_nanosic_hw_lock);
bool g_nanosic_hw_ready;
bool g_nanosic_hw_recovering;
bool g_nanosic_hw_suspended;

#define SUPPORT_GPIO_SLEEP_FUNCTION
#define NANOSIC_GPIO_BASE 301
#define NANOSIC_GPIO_OFFSET_MAX 255
#define NANOSIC_WAKE_IRQ_FLAGS (IRQF_TRIGGER_RISING | IRQF_ONESHOT)

static bool nanosic_gpio_available_locked(void)
{
    lockdep_assert_held(&g_nanosic_hw_lock);

    return registered && g_nanosic_hw_ready &&
           !g_nanosic_hw_recovering && !g_nanosic_hw_suspended;
}

static int nanosic_gpio_safe_levels_locked(void)
{
    int first_err = 0;
    int err;

    lockdep_assert_held(&g_nanosic_hw_lock);

    if (gpio_reset_desc) {
        err = gpiod_direction_output_raw(gpio_reset_desc, 0);
        if (err)
            first_err = err;
    }
    if (gpio_sleep_desc) {
        err = gpiod_direction_output_raw(gpio_sleep_desc, 0);
        if (err && !first_err)
            first_err = err;
        else if (!err)
            sleep_pin_status = 0;
    }
    if (gpio_vdd_desc) {
        err = gpiod_direction_output_raw(gpio_vdd_desc, 0);
        if (err && !first_err)
            first_err = err;
    }

    return first_err;
}

/** ************************************************************************//**
 * @brief   Nanosic_wakeup_irq
 *          GPIO 唤醒中断处理程序
 ** */
static irqreturn_t
Nanosic_wakeup_irq(int irq, void *dev_id)
{
    struct nano_i2c_client *i2c_client = READ_ONCE(gI2c_client);

    if (!READ_ONCE(g_panel_status) && i2c_client && i2c_client->dev &&
        i2c_client->input_dev) {
        pm_wakeup_event(i2c_client->dev, 1000);

        spin_lock(&i2c_client->input_report_lock);

        input_report_key(i2c_client->input_dev,KEY_WAKEUP,1);
        input_sync(i2c_client->input_dev);
        input_report_key(i2c_client->input_dev,KEY_WAKEUP,0);
        input_sync(i2c_client->input_dev);

        spin_unlock(&i2c_client->input_report_lock);
        dbgprint(ALERT_LEVEL,"Nanosic_wakeup_irq wakeup panel\n");
    }

	return IRQ_HANDLED;
}

/** ************************************************************************//**
 *  @func Nanosic_GPIO_register
 *
 *  @brief gpio申请及配置
 *
 ** */
int
Nanosic_GPIO_register(int vdd_pin,int reset_pin, int status_pin, int irq_pin,int sleep_pin)
{
    bool irq_requested = false;
    bool reset_requested = false;
    bool sleep_requested = false;
    bool status_requested = false;
    bool vdd_requested = false;
    int err;

    if (!gpio_is_valid(vdd_pin) || !gpio_is_valid(reset_pin) ||
        !gpio_is_valid(status_pin) || !gpio_is_valid(irq_pin) ||
        !gpio_is_valid(sleep_pin)) {
        dbgprint(ERROR_LEVEL,"invalid pin value\n");
        return -EINVAL;
    }

    mutex_lock(&g_nanosic_hw_lock);
    if (registered) {
        err = -EBUSY;
        goto out_unlock;
    }

    g_nanosic_hw_ready = false;
    g_nanosic_hw_recovering = false;
    g_nanosic_hw_suspended = false;
    wakeup_irq_registered = false;
    sleep_pin_status = -1;

    gpio_reset_desc = gpio_to_desc(reset_pin);
    gpio_status_desc = gpio_to_desc(status_pin);
    gpio_vdd_desc = gpio_to_desc(vdd_pin);
    gpio_irq_desc = gpio_to_desc(irq_pin);
    gpio_sleep_desc = gpio_to_desc(sleep_pin);
    if (IS_ERR_OR_NULL(gpio_reset_desc) ||
        IS_ERR_OR_NULL(gpio_status_desc) ||
        IS_ERR_OR_NULL(gpio_vdd_desc) ||
        IS_ERR_OR_NULL(gpio_irq_desc) ||
        IS_ERR_OR_NULL(gpio_sleep_desc)) {
        err = -ENODEV;
        goto err_clear;
    }

    gpio_reset_pin = reset_pin;
    gpio_status_pin = status_pin;
    gpio_vdd_pin = vdd_pin;
    gpio_irq_pin = irq_pin;
    gpio_sleep_pin = sleep_pin;
    dbgprint(ERROR_LEVEL,"Nanosic_GPIO_register :gpio_reset_pin:%d,gpio_status_pin:%d,gpio_vdd_pin:%d,gpio_irq_pin:%d\n",gpio_reset_pin,gpio_status_pin,gpio_vdd_pin,gpio_irq_pin);

    err = gpio_request(gpio_reset_pin,"nanosic-reset");
    if (err)
        goto err_clear;
    reset_requested = true;

    err = gpio_request(gpio_sleep_pin,"nanosic-sleep");
    if (err)
        goto err_free;
    sleep_requested = true;

    err = gpio_request(gpio_status_pin,"nanosic-status");
    if (err)
        goto err_free;
    status_requested = true;

    err = gpio_request(gpio_irq_pin,"nanosic-irq");
    if (err)
        goto err_free;
    irq_requested = true;

    err = gpio_request(gpio_vdd_pin,"nanosic-vdd");
    if (err)
        goto err_free;
    vdd_requested = true;

    err = gpiod_direction_output_raw(gpio_reset_desc,0);
    if (err)
        goto err_safe;
    err = gpiod_direction_output_raw(gpio_sleep_desc,0);
    if (err)
        goto err_safe;
    err = gpiod_direction_input(gpio_status_desc);
    if (err)
        goto err_safe;
    err = gpiod_direction_input(gpio_irq_desc);
    if (err)
        goto err_safe;
    err = gpiod_direction_output_raw(gpio_vdd_desc,1);
    if (err)
        goto err_safe;

    msleep(2);
    err = gpiod_direction_output_raw(gpio_reset_desc, 1);
    if (err)
        goto err_safe;
    err = gpiod_direction_output_raw(gpio_sleep_desc, 1);
    if (err)
        goto err_safe;
    sleep_pin_status = 1;

    g_wakeup_irqno = gpiod_to_irq(gpio_status_desc);
    if (g_wakeup_irqno < 0) {
        err = g_wakeup_irqno;
        goto err_safe;
    }
    err = request_threaded_irq(g_wakeup_irqno, NULL, Nanosic_wakeup_irq,
                               NANOSIC_WAKE_IRQ_FLAGS, "8030_io_wakeup",
                               &gpio_status_pin);
    if (err)
        goto err_safe;

    wakeup_irq_registered = true;
    registered = true;
    g_nanosic_hw_ready = true;
    g_nanosic_hw_recovering = false;
    g_nanosic_hw_suspended = false;
    WRITE_ONCE(g_panel_status, true);
    mutex_unlock(&g_nanosic_hw_lock);

    dbgprint(ERROR_LEVEL,"register gpio succeed\n");
    return 0;

err_safe:
    if (nanosic_gpio_safe_levels_locked())
        dbgprint(ERROR_LEVEL,"failed to put GPIOs in safe state\n");
err_free:
    if (vdd_requested)
        gpio_free(gpio_vdd_pin);
    if (irq_requested)
        gpio_free(gpio_irq_pin);
    if (status_requested)
        gpio_free(gpio_status_pin);
    if (sleep_requested)
        gpio_free(gpio_sleep_pin);
    if (reset_requested)
        gpio_free(gpio_reset_pin);
err_clear:
    gpio_reset_desc = NULL;
    gpio_status_desc = NULL;
    gpio_vdd_desc = NULL;
    gpio_irq_desc = NULL;
    gpio_sleep_desc = NULL;
    gpio_reset_pin = -1;
    gpio_status_pin = -1;
    gpio_vdd_pin = -1;
    gpio_irq_pin = -1;
    gpio_sleep_pin = -1;
    g_wakeup_irqno = -1;
    sleep_pin_status = -1;
    wakeup_irq_registered = false;
out_unlock:
    mutex_unlock(&g_nanosic_hw_lock);
    return err;
}

/** ************************************************************************//**
 *  @func Nanosic_GPIO_recovery
 *
 *  @brief 操作gpio时序使803进入出厂恢复
 *
 ** */
#if 0
int
Nanosic_GPIO_recovery(struct nano_i2c_client* client , char* data, int datalen)
{
    int err = -1;
    int result = -EIO;
    char readbuf[I2C_DATA_LENGTH_READ]={0};
    int read_retry = 30;
    int write_retry = 30;

    if(IS_ERR_OR_NULL(data) || IS_ERR_OR_NULL(client) || datalen <= 0){
        dbgprint(ERROR_LEVEL,"fail recovery reason invalid argment\n");
        return err;
    }

    if(registered == false){
        dbgprint(ERROR_LEVEL,"need register first\n");
        return err;
    }

    if(client->irqno <= 0) {
        dbgprint(INFO_LEVEL,"Nanosic_GPIO_recovery: Nanosic_timer_release\n");
        Nanosic_timer_release();							/*定时器测试模式,中断不使用情况下使用*/
    }else if (client->irq_registered) {
        dbgprint(INFO_LEVEL,"Nanosic_GPIO_recovery: free_irq\n");
        free_irq(client->irqno,client);				/*irq需要切换成input模式,这里先关闭中断,结束时再次注册*/
        client->irq_registered = false;
    }

    Nanosic_GPIO_irq_release();
    Nanosic_workQueue_flush(client->worker);

    gpiod_set_raw_value(gpio_to_desc(gpio_reset_pin),0);	/*output 0 from reset pin*/

    mdelay(2);
    /*status/sleep/irq switch to output mode*/
    gpiod_direction_output_raw(gpio_to_desc(gpio_status_pin),0);	/*output 0 from status pin*/
    gpiod_direction_output_raw(gpio_to_desc(gpio_sleep_pin),0);	/*output 0 from sleep pin */
    gpiod_direction_output_raw(gpio_to_desc(gpio_irq_pin),0);	/*output 0 from irq pin */

    mdelay(2);

    gpiod_set_raw_value(gpio_to_desc(gpio_status_pin),1);	/*output 1 from status pin*/
    gpiod_set_raw_value(gpio_to_desc(gpio_sleep_pin),1);	/*output 1 from sleep pin*/

    mdelay(2);							/*等待sleep/status输出电平稳定*/

    gpiod_set_raw_value(gpio_to_desc(gpio_reset_pin),1);	/*output 1 from reset pin*/
    dbgprint(DEBUG_LEVEL,"control 803 reset\n");

    /*延时350ms,等待803进入boot模式*/
    mdelay(350);

    while(read_retry > 0)
    {   /*查询803是否进入boot模式,读3次*/
        err = Nanosic_i2c_read(client,readbuf,sizeof(readbuf));
        result = err;
        if(err > 0){
            rawdata_show("boot recv",readbuf,sizeof(readbuf));
            break;
        }
        mdelay(30);
        read_retry--;
        dbgprint(ERROR_LEVEL,"i2c read retry %d\n",read_retry);
    }

    if(err > 0)/*已经进入boot模式*/
    {
        mdelay(2);
        while(write_retry > 0)
        {
            dbgprint(DEBUG_LEVEL,"send recovery command\n");
            /*803 reset后有500ms的i2c通讯窗口期,需要在500ms之内发送恢复出厂command指令*/
            err = Nanosic_i2c_write(client,data,datalen);
            result = err;
            dbgprint(DEBUG_LEVEL,"i2c write ret=%d\n",err);
            if(err > 0){
                dbgprint(DEBUG_LEVEL,"send recovery command succeed\n");
                break;
            }
            mdelay(30);
            write_retry--;
            dbgprint(DEBUG_LEVEL,"i2c write fail then retry %d\n",write_retry);
        }
    }

    gpiod_direction_input(gpio_to_desc(gpio_status_pin));	/*switch to input mode*/
    gpiod_direction_input(gpio_to_desc(gpio_irq_pin));		/*switch to input mode*/
    gpiod_set_raw_value(gpio_to_desc(gpio_sleep_pin),1);	/*sleep继续保持高电平*/

    if(client->irqno <= 0) {
        dbgprint(ERROR_LEVEL,"Nanosic_GPIO_recovery: Nanosic_timer_register\n");
        Nanosic_timer_register(client);
    }else{																		/*重新注册中断*/
        err = request_threaded_irq(client->irqno, NULL, Nanosic_i2c_irq, client->irqflags, "8030_io_irq", client);
        if (err < 0) {
            dbgprint(ERROR_LEVEL,"Could not register for %s interrupt, irq = %d, ret = %d, irqflags = 0x%x\n","8030_io_irq", client->irqno, err,client->irqflags);
            result = err;
        } else
            client->irq_registered = true;
    }

    if(gpio_is_valid(gpio_status_pin))
    {
        g_wakeup_irqno = gpio_to_irq(gpio_status_pin);
        err = request_threaded_irq(g_wakeup_irqno, NULL, Nanosic_wakeup_irq, IRQF_TRIGGER_FALLING | IRQF_ONESHOT | IRQF_NO_SUSPEND, "8030_io_wakeup", (void*)&gpio_status_pin);
        if (err < 0) {
            dbgprint(ERROR_LEVEL,"Could not register for %s interrupt, irq = %d, ret = %d\n","8030_io_wakeup", g_wakeup_irqno, err);
            result = err;
        } else
            wakeup_irq_registered = true;
    }

    return result;
}
#endif

static void nanosic_gpio_irq_release_locked(void)
{
    lockdep_assert_held(&g_nanosic_hw_lock);

    if (!wakeup_irq_registered)
        return;

    free_irq(g_wakeup_irqno, &gpio_status_pin);
    wakeup_irq_registered = false;
}

static int nanosic_gpio_irq_register_locked(void)
{
    int err;

    lockdep_assert_held(&g_nanosic_hw_lock);
    if (!gpio_status_desc)
        return -ENODEV;

    g_wakeup_irqno = gpiod_to_irq(gpio_status_desc);
    if (g_wakeup_irqno < 0)
        return g_wakeup_irqno;

    err = request_threaded_irq(g_wakeup_irqno, NULL, Nanosic_wakeup_irq,
                               NANOSIC_WAKE_IRQ_FLAGS, "8030_io_wakeup",
                               &gpio_status_pin);
    if (!err)
        wakeup_irq_registered = true;

    return err;
}

int Nanosic_GPIO_recovery(struct nano_i2c_client *client, char *data,
                          int datalen)
{
    bool main_irq_registered;
    bool wakeup_irq_was_registered;
    int restore_err = 0;
    int err;
    int result = -EIO;
    char readbuf[I2C_DATA_LENGTH_READ] = { 0 };
    int read_retry = 30;
    int write_retry = 30;

    if (IS_ERR_OR_NULL(data) || IS_ERR_OR_NULL(client) ||
        datalen != I2C_DATA_LENGTH_WRITE)
        return -EINVAL;

    /* Pin client storage until recovery has restored or disabled all IRQs. */
    mutex_lock(&g_nanosic_lifecycle_lock);
    mutex_lock(&g_nanosic_hw_lock);
    if (!registered || !g_nanosic_hw_ready || client != gI2c_client) {
        result = -ENODEV;
        goto out_unlock_state;
    }
    if (g_nanosic_hw_recovering) {
        result = -EBUSY;
        goto out_unlock_state;
    }
    if (g_nanosic_hw_suspended) {
        result = -EHOSTDOWN;
        goto out_unlock_state;
    }
    g_nanosic_hw_recovering = true;
    main_irq_registered = client->irq_registered;
    wakeup_irq_was_registered = wakeup_irq_registered;
    mutex_unlock(&g_nanosic_hw_lock);

    if (main_irq_registered) {
        free_irq(client->irqno, client);
        mutex_lock(&g_nanosic_hw_lock);
        client->irq_registered = false;
        mutex_unlock(&g_nanosic_hw_lock);
    }

    Nanosic_GPIO_irq_release();
    Nanosic_workQueue_flush(client->worker);
    Nanosic_input_release();
    WRITE_ONCE(gHallStatus, 0);

    mutex_lock(&g_nanosic_hw_lock);
    if (!registered || !g_nanosic_hw_ready || client != gI2c_client) {
        result = -ENODEV;
        restore_err = result;
        goto restore_irqs;
    }

    err = gpiod_direction_output_raw(gpio_reset_desc, 0);
    if (err) {
        result = err;
        goto restore_directions;
    }
    mdelay(2);

    err = gpiod_direction_output_raw(gpio_status_desc, 0);
    if (err) {
        result = err;
        goto restore_directions;
    }
    err = gpiod_direction_output_raw(gpio_sleep_desc, 0);
    if (err) {
        result = err;
        goto restore_directions;
    }
    sleep_pin_status = 0;
    err = gpiod_direction_output_raw(gpio_irq_desc, 0);
    if (err) {
        result = err;
        goto restore_directions;
    }

    mdelay(2);
    err = gpiod_direction_output_raw(gpio_status_desc, 1);
    if (err) {
        result = err;
        goto restore_directions;
    }
    err = gpiod_direction_output_raw(gpio_sleep_desc, 1);
    if (err) {
        result = err;
        goto restore_directions;
    }
    sleep_pin_status = 1;
    mdelay(2);
    err = gpiod_direction_output_raw(gpio_reset_desc, 1);
    if (err) {
        result = err;
        goto restore_directions;
    }
    mdelay(350);

    while (read_retry-- > 0) {
        err = Nanosic_i2c_read_locked(client, readbuf, sizeof(readbuf));
        result = err;
        if (err == (int)sizeof(readbuf))
            break;
        mdelay(30);
    }

    if (err == (int)sizeof(readbuf)) {
        mdelay(2);
        while (write_retry-- > 0) {
            err = Nanosic_i2c_write_locked(client, data, datalen);
            result = err;
            if (err == datalen)
                break;
            mdelay(30);
        }
    }

restore_directions:
    err = gpiod_direction_input(gpio_status_desc);
    if (err && !restore_err)
        restore_err = err;
    err = gpiod_direction_input(gpio_irq_desc);
    if (err && !restore_err)
        restore_err = err;
    err = gpiod_direction_output_raw(gpio_sleep_desc, 1);
    if (err && !restore_err)
        restore_err = err;
    else if (!err)
        sleep_pin_status = 1;

restore_irqs:
    if (main_irq_registered && !restore_err) {
        err = request_threaded_irq(client->irqno, NULL, Nanosic_i2c_irq,
                                   client->irqflags, "8030_io_irq", client);
        if (err)
            restore_err = err;
        else
            client->irq_registered = true;
    }

    if (wakeup_irq_was_registered && !restore_err) {
        err = nanosic_gpio_irq_register_locked();
        if (err)
            restore_err = err;
    }

    if (!restore_err && client->irq_registered) {
        err = Nanosic_i2c_request_status_locked(client);
        if (err != I2C_DATA_LENGTH_WRITE) {
            dbgprint(ERROR_LEVEL, "recovery status query failed: %d\n", err);
            result = err < 0 ? err : -EIO;
        }
    }

    if (restore_err) {
        if (client->irq_registered) {
            free_irq(client->irqno, client);
            client->irq_registered = false;
        }
        nanosic_gpio_irq_release_locked();
        g_nanosic_hw_ready = false;
        if (nanosic_gpio_safe_levels_locked())
            dbgprint(ERROR_LEVEL,"failed to put GPIOs in safe state\n");
    }

    g_nanosic_hw_recovering = false;
    if (restore_err)
        result = restore_err;
    mutex_unlock(&g_nanosic_hw_lock);
    mutex_unlock(&g_nanosic_lifecycle_lock);
    return result;

out_unlock_state:
    mutex_unlock(&g_nanosic_hw_lock);
    mutex_unlock(&g_nanosic_lifecycle_lock);
    return result;
}

/** ************************************************************************//**
 *  @func Nanosic_GPIO_sleep
 *
 *  @brief switch to sleep mode , low level valid 低电平有效
 *  bool sleep : 0 -> 进入睡眠模式
 *               1 -> 退出睡眠模式
 ** */
int Nanosic_GPIO_sleep_locked(bool awake)
{
    int err;

    lockdep_assert_held(&g_nanosic_hw_lock);

    if (!nanosic_gpio_available_locked())
        return g_nanosic_hw_suspended ? -EHOSTDOWN : -ENODEV;
    if (!gpio_sleep_desc)
        return -ENODEV;
    if (sleep_pin_status == awake)
        return 0;

    err = gpiod_direction_output_raw(gpio_sleep_desc, awake);
    if (err)
        return err;

    sleep_pin_status = awake;
    dbgprint(INFO_LEVEL,"set gpio sleep pin %d\n",awake);
    return 0;
}

int Nanosic_GPIO_sleep(bool awake)
{
    int err;

    mutex_lock(&g_nanosic_hw_lock);
    err = Nanosic_GPIO_sleep_locked(awake);
    mutex_unlock(&g_nanosic_hw_lock);

    return err;
}

int
Nanosic_GPIO_irqget(void)
{
    return READ_ONCE(gpio_irq_pin);
}

int Nanosic_GPIO_irq_level_locked(void)
{
    lockdep_assert_held(&g_nanosic_hw_lock);

    if (!nanosic_gpio_available_locked())
        return g_nanosic_hw_suspended ? -EHOSTDOWN : -ENODEV;
    if (!gpio_irq_desc)
        return -ENODEV;

    return gpiod_get_raw_value(gpio_irq_desc);
}

int Nanosic_GPIO_reset_locked(void)
{
    int err;

    lockdep_assert_held(&g_nanosic_hw_lock);

    if (!nanosic_gpio_available_locked())
        return g_nanosic_hw_suspended ? -EHOSTDOWN : -ENODEV;
    if (!gpio_reset_desc)
        return -ENODEV;

    err = gpiod_direction_output_raw(gpio_reset_desc, 0);
    if (err)
        return err;
    mdelay(100);

    err = gpiod_direction_output_raw(gpio_reset_desc, 1);
    if (err) {
        g_nanosic_hw_ready = false;
        return err;
    }

    mdelay(500);
    dbgprint(ALERT_LEVEL,"reset wn8030\n");
    return 0;
}

int Nanosic_GPIO_reset(void)
{
    int err;

    mutex_lock(&g_nanosic_hw_lock);
    err = Nanosic_GPIO_reset_locked();
    mutex_unlock(&g_nanosic_hw_lock);

    return err;
}

/** ************************************************************************//**
 *  @func Nanosic_GPIO_test
 *
 *  @brief set gpio pin level
 *
 ** */
int Nanosic_GPIO_set(unsigned int gpio_offset, bool gpio_level)
{
    struct gpio_desc *desc;
    int gpio_pin;
    int err;

    if (gpio_offset > NANOSIC_GPIO_OFFSET_MAX)
        return -ERANGE;

    gpio_pin = NANOSIC_GPIO_BASE + gpio_offset;
    if (!gpio_is_valid(gpio_pin))
        return -EINVAL;

    desc = gpio_to_desc(gpio_pin);
    if (!desc)
        return -ENODEV;

    mutex_lock(&g_nanosic_hw_lock);
    if (!nanosic_gpio_available_locked()) {
        err = g_nanosic_hw_suspended ? -EHOSTDOWN : -ENODEV;
        goto out_unlock;
    }

    err = gpiod_direction_output_raw(desc, gpio_level);
    if (!err)
        dbgprint(DEBUG_LEVEL,"set gpio pin %d level %d\n",gpio_pin,
                 gpio_level);

out_unlock:
    mutex_unlock(&g_nanosic_hw_lock);
    return err;
}

/** ************************************************************************//**
 *  @func Nanosic_RequestGensor_notify
 *
 *  @brief request fw get gsensor
 *
 ** */
int
Nanosic_RequestGensor_notify(void)
{
    int err;
    unsigned char requestgensor_data[66] = {0x32,0x00,0x4E,0x31,FIELD_HOST,FIELD_176X,0x52,0x00,0x89};

    err = Nanosic_i2c_write(gI2c_client,requestgensor_data,sizeof(requestgensor_data));

    dbgprint(INFO_LEVEL,"Nanosic_RequestGensor_notify, err:%d report 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
        err,requestgensor_data[0],requestgensor_data[1],requestgensor_data[2],requestgensor_data[3],requestgensor_data[4],requestgensor_data[5],requestgensor_data[6],requestgensor_data[7],requestgensor_data[8],requestgensor_data[9],requestgensor_data[10]);

    return err;
}


/** ************************************************************************//**
 *  @func Nanosic_Hall_notify
 *
 ** */
int
Nanosic_Hall_notify(int hall_n_pin, int hall_s_pin)
{
    unsigned char hall_data[I2C_DATA_LENGTH_WRITE] = {0};
    int hall_n;
    int hall_s;
    int ret;

    if (hall_n_pin < 0 || hall_s_pin < 0) {
        dbgprint(ERROR_LEVEL,"hall_n:%d hall_s:%d\n", hall_n_pin, hall_s_pin);
        return -ENODEV;
    }

    hall_n = gpiod_get_raw_value(gpio_to_desc(hall_n_pin));
    hall_s = gpiod_get_raw_value(gpio_to_desc(hall_s_pin));
    if (hall_n < 0 || hall_s < 0) {
        dbgprint(ERROR_LEVEL,"failed to read hall GPIOs: %d %d\n", hall_n, hall_s);
        return -ENODEV;
    }

    hall_data[0] = 0x24;
    hall_data[1] = 0x20;
    hall_data[2] = 0x80;
    hall_data[3] = 0x80;
    hall_data[4] = 0xE1;
    hall_data[5] = 0x01;
    if (hall_n == 0 && hall_s == 1)
        hall_data[6] = 0x01;
    else if (hall_n == 1 && hall_s == 0)
        hall_data[6] = 0x10;
    else if (hall_n == 1 && hall_s == 1)
        hall_data[6] = 0x11;

    dbgprint(ERROR_LEVEL,"hall_n:%d hall_s:%d state:0x%02x\n",
             hall_n, hall_s, hall_data[6]);

    ret = Nanosic_chardev_client_write((char *)hall_data, sizeof(hall_data));
    dbgprint(ERROR_LEVEL,
             "Hall notify, err:%d report 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
             ret, hall_data[0], hall_data[1], hall_data[2], hall_data[3],
             hall_data[4], hall_data[5], hall_data[6]);

    return ret;
}


/** ************************************************************************//**
 *  @func Nanosic_GPIO_irq_release
 *
 *  @brief release the status wake IRQ before freeing shared I2C state
 *
 ** */
void
Nanosic_GPIO_irq_release(void)
{
    mutex_lock(&g_nanosic_hw_lock);
    nanosic_gpio_irq_release_locked();
    mutex_unlock(&g_nanosic_hw_lock);
}

bool Nanosic_GPIO_irq_registered_locked(void)
{
    lockdep_assert_held(&g_nanosic_hw_lock);

    return wakeup_irq_registered;
}

/** ************************************************************************//**
 *  @func Nanosic_GPIO_release
 *
 *  @brief gpio free
 *
 ** */
void
Nanosic_GPIO_release(void)
{
    mutex_lock(&g_nanosic_hw_lock);
    if (!registered)
        goto out_unlock;

    g_nanosic_hw_ready = false;
    g_nanosic_hw_recovering = false;
    g_nanosic_hw_suspended = false;
    WRITE_ONCE(g_panel_status, false);

    nanosic_gpio_irq_release_locked();
    if (nanosic_gpio_safe_levels_locked())
        dbgprint(ERROR_LEVEL,"failed to put GPIOs in safe state\n");

    gpio_free(gpio_irq_pin);
    gpio_free(gpio_sleep_pin);
    gpio_free(gpio_status_pin);
    gpio_free(gpio_reset_pin);
    gpio_free(gpio_vdd_pin);

    registered = false;
    gpio_reset_desc = NULL;
    gpio_status_desc = NULL;
    gpio_vdd_desc = NULL;
    gpio_irq_desc = NULL;
    gpio_sleep_desc = NULL;
    gpio_reset_pin = -1;
    gpio_status_pin = -1;
    gpio_vdd_pin = -1;
    gpio_irq_pin = -1;
    gpio_sleep_pin = -1;
    g_wakeup_irqno = -1;
    sleep_pin_status = -1;

out_unlock:
    mutex_unlock(&g_nanosic_hw_lock);
}
