/** ***************************************************************************
 * @file nano_sysfs.c
 *
 * @brief Create device sysfs node
 *        Create /sys/class/nanodev/nanodev0/_debuglevel
 *        Create /sys/class/nanodev/nanodev0/_schedule
 *        Create /sys/class/nanodev/nanodev0/_versioncode
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

#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/sysfs.h>
#include "nano_macro.h"

int debuglevel = INFO_LEVEL;

/** ************************************************************************//**
 * @func debuglevel_show
 *
 * @brief null
 ** */
static ssize_t
debuglevel_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    return sprintf(buf,"debuglevel=%d\n",debuglevel);
}

/** ************************************************************************//**
 * @func debuglevel_show
 *
 * @brief null
 ** */
static ssize_t
debuglevel_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int i;
    char debuglevel_data[16] = {0x27, 0x00, 0xFF, 0xFF};

	ret = kstrtouint(buf, 10, &i);
    if(ret)
        return 0;

    debuglevel = i;
    debuglevel_data[1] = (char)i;
    Nanosic_chardev_client_write(debuglevel_data, 16);

    return count;
}

/** ************************************************************************//**
 * @func schedule_show
 *
 * @brief null
 ** */
static ssize_t
schedule_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    struct nano_i2c_client* i2c_client = gI2c_client;

    if(!IS_ERR_OR_NULL(i2c_client) && !IS_ERR_OR_NULL(i2c_client->worker))
        return sprintf(buf,"schedule=%d i2c-read=%d i2c-read-error=%d\n",
                atomic_read(&i2c_client->worker->schedule_count),atomic_read(&i2c_client->i2c_read_count),
                atomic_read(&i2c_client->i2c_error_count));
    else
        return sprintf(buf,"schedule error\n");
}

/** ************************************************************************//**
 * @func schedule_store
 *
 * @brief 通过echo 5 >  /sys/class/nanodev/nanodev0/_schedule 方式来模拟i2c中断的次数,并执行5次i2c_read
 ** */
static ssize_t
schedule_store(struct device *dev, struct device_attribute *attr,const char *buf, size_t count)
{
	int ret;
	unsigned int i;
	unsigned int schedule_count;
	struct nano_i2c_client* i2c_client = gI2c_client;

	ret = kstrtouint(buf, 10, &i);
    if(ret)
        return 0;

    if(IS_ERR_OR_NULL(i2c_client))
        return count;

    schedule_count = i> 10000 ? 10000 : i;

    while(schedule_count)
    {
        Nanosic_workQueue_schedule(i2c_client->worker);
        schedule_count--;
    }

//    Nanosic_GPIO_set(i?true:false);

    return count;
}

/** ************************************************************************//**
 * @func version_code_show
 *
 * @brief null
 ** */
static ssize_t
version_SDK_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    return sprintf(buf,"SDK %s\n",DRV_VERSION);
}


/** ************************************************************************//**
 * @func version_code_store
 *
 * @brief 查看驱动版本号
 ** */
static ssize_t
version_SDK_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    return count;
}

/** ************************************************************************//**
 * @func version_803x_show
 *
 * @brief 查看803x版本
 ** */
static ssize_t
version_803x_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    return sprintf(buf,"version803x=%s\n",strlen(gVers803x)>0?gVers803x:"null");
}

/** ************************************************************************//**
 * @func version_803x_store
 *
 * @brief 发送读803x版本命令
 ** */
static ssize_t
version_803x_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct nano_i2c_client* i2c_client = gI2c_client;

    if(i2c_client)
    {
        int i=2;
        char read_803_vers_cmd[I2C_DATA_LENGTH_WRITE]={0x32,0x00,0x4F,0x30,0x80,FIELD_803X,0x01,0x00,0x00};
        memset(gVers803x,0,sizeof(gVers803x));
        for(;i<8;i++)
        {
            read_803_vers_cmd[8] +=read_803_vers_cmd[i];/*cal sum*/
        }
        rawdata_show("request 803 version",read_803_vers_cmd,sizeof(read_803_vers_cmd));
        Nanosic_i2c_write(i2c_client,read_803_vers_cmd,sizeof(read_803_vers_cmd));
    }

    return count;
}

/** ************************************************************************//**
 * @func version_176x_show
 *
 * @brief 查看keypad版本命令
 ** */
static ssize_t
version_176x_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    bool keypad_conneted = (gHallStatus>>0)&0x1;
    bool keypad_power    = (gHallStatus>>1)&0x1;
    bool keypad_POGOPIN  = (gHallStatus>>6)&0x1;

    if(keypad_conneted & keypad_power){
        sprintf(buf,"Connected=[%d] Power=[%d] POGOPIN=[%s] versionKeyPad=[%s]\n",keypad_conneted,keypad_power,keypad_POGOPIN ?"ERROR":"OK",strlen(gVers176x)>0?gVers176x:"null");
    }else{
        sprintf(buf,"Connected=[%d] Power=[%d]\n",keypad_conneted,keypad_power);
    }

    return strlen(buf);
}

/** ************************************************************************//**
 * @func version_176x_store
 *
 * @brief 发送读keypad版本命令
 ** */
static ssize_t
version_176x_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct nano_i2c_client* i2c_client = gI2c_client;

    if(i2c_client)
    {
        int i=0;

        char read_keypad_status[I2C_DATA_LENGTH_WRITE]={0x32,0x00,0x4E,0x31,FIELD_HOST,FIELD_176X,0xA1,0x01,0x01,0x00};
        char read_keypad_vers_cmd[I2C_DATA_LENGTH_WRITE]={0x32,0x00,0x4F,0x30,FIELD_HOST,FIELD_176X,0x01,0x00};
        gHallStatus = 0;

        for(i=2;i<9;i++)
            read_keypad_status[9] +=read_keypad_status[i];/*cal sum*/
        rawdata_show("request keypad hall status",read_keypad_status,sizeof(read_keypad_status));
        Nanosic_i2c_write(i2c_client,read_keypad_status,sizeof(read_keypad_status));

        msleep(10);

        memset(gVers176x,0,sizeof(gVers176x));
        for(i=2;i<8;i++)
            read_keypad_vers_cmd[8] +=read_keypad_vers_cmd[i];/*cal sum*/
        rawdata_show("request keypad version",read_keypad_vers_cmd,sizeof(read_keypad_vers_cmd));
        Nanosic_i2c_write(i2c_client,read_keypad_vers_cmd,sizeof(read_keypad_vers_cmd));
    }

    return count;
}

/** ************************************************************************//**
 * @func sleep_803x_show
 *
 * @brief 查看803x版本
 ** */
static ssize_t
sleep_803x_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    return sprintf(buf,"sleep 803x\n");
}

/** ************************************************************************//**
 * @func sleep_803x_store
 *
 * @brief 发送读803x版本命令
 ** */
static ssize_t
sleep_803x_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int i;

    ret = kstrtouint(buf, 16, &i);
    if(ret)
        return ret;

    ret = Nanosic_GPIO_sleep(i > 0);

    return ret ? ret : count;
}

/** ************************************************************************//**
 * @func gpio_set_show
 *
 * @brief gpio set help
 ** */
static ssize_t
gpio_set_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    return sprintf(buf,"usage echo pin level > _gpioset\n");
}

/** ************************************************************************//**
 * @func gpio_set_store
 *
 * @brief 设置gpio pin电压
 ** */
static ssize_t
gpio_set_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    unsigned int gpio_offset;
    unsigned int gpio_value;
    int ret;

    if (sscanf(buf, "%u %u", &gpio_offset, &gpio_value) != 2)
        return -EINVAL;
    if (gpio_value > 1)
        return -EINVAL;

    ret = Nanosic_GPIO_set(gpio_offset, gpio_value);
    return ret ? ret : count;
}

/** ************************************************************************//**
 * @func debuglevel_show
 *
 * @brief null
 ** */
static ssize_t
dispatch_keycode_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    return sprintf(buf,"dispatch_keycode_show\n");
}

/** ************************************************************************//**
 * @func debuglevel_show
 *
 * @brief write keycode to input system for test
 ** */
static ssize_t
dispatch_keycode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int i;
    unsigned char down[12] = {0x57,0x00,0x39,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    unsigned char up[12]   = {0x57,0x00,0x39,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

	ret = kstrtouint(buf, 16, &i);
    if(ret)
        return 0;

    down[6] = i;

    Nanosic_i2c_parse(down,sizeof(down));
    Nanosic_i2c_parse(up,sizeof(up));

    return count;
}

/** ************************************************************************//**
 * @func _reset8030
 *
 * @brief null
 ** */
static ssize_t
reset_8030_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    return sprintf(buf, "Usage: echo 1 > _reset8030\n");
}

/** ************************************************************************//**
 * @func _reset8030
 *
 * @brief reset wn8030
 ** */
static ssize_t
reset_8030_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int ret;
    unsigned int cmd;

    ret = kstrtouint(buf, 10, &cmd);
    if(ret)
        return ret;

    if(cmd != 1)
        return -EINVAL;

    ret = Nanosic_GPIO_reset();
    return ret ? ret : count;
}

/*设置调试级别*/
static DEVICE_ATTR(_debuglevel, 0600, debuglevel_show, debuglevel_store);

/*统计workqueue运行次数*/
static DEVICE_ATTR(_schedule, 0600, schedule_show, schedule_store);

/*查看sdk版本号*/
static DEVICE_ATTR(_versionSDK, 0600, version_SDK_show, version_SDK_store);

/*键盘测试*/
static DEVICE_ATTR(_keycode, 0600, dispatch_keycode_show, dispatch_keycode_store);

/*查看803版本号*/
static DEVICE_ATTR(_version803x, 0600, version_803x_show, version_803x_store);

/*查看keypad版本号*/
static DEVICE_ATTR(_version176x, 0600, version_176x_show, version_176x_store);

/*控制803睡眠*/
static DEVICE_ATTR(_sleep803x, 0600, sleep_803x_show, sleep_803x_store);

/*gpio set method*/
static DEVICE_ATTR(_gpioset, 0600, gpio_set_show, gpio_set_store);

/*reset 8030*/
static DEVICE_ATTR(_reset8030, 0600, reset_8030_show, reset_8030_store);

static struct attribute *nanosic_sysfs_attrs[] = {
    &dev_attr__debuglevel.attr,
    &dev_attr__schedule.attr,
    &dev_attr__versionSDK.attr,
    &dev_attr__keycode.attr,
    &dev_attr__version803x.attr,
    &dev_attr__version176x.attr,
    &dev_attr__sleep803x.attr,
    &dev_attr__gpioset.attr,
    &dev_attr__reset8030.attr,
    NULL,
};

static const struct attribute_group nanosic_sysfs_group = {
    .attrs = nanosic_sysfs_attrs,
};

/** ************************************************************************//**
 * @func Nanosic_Sysfs_create
 *
 * @brief create sysfs node for nanosic i2c-hid driver
 */
int
Nanosic_sysfs_create(struct device* dev)
{
    if (IS_ERR_OR_NULL(dev))
        return -EINVAL;

    return sysfs_create_group(&dev->kobj, &nanosic_sysfs_group);
}

/** ************************************************************************//**
 * @func Nanosic_Sysfs_release
 *
 * @brief
 */
void
Nanosic_sysfs_release(struct device* dev)
{
    if (!IS_ERR_OR_NULL(dev))
        sysfs_remove_group(&dev->kobj, &nanosic_sysfs_group);
}
