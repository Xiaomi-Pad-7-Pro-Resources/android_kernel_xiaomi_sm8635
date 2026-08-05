/** ***************************************************************************
 * @file nano_input.c
 *
 * @brief implent nanosic virtual input driver such as keyboard , mouse and touch
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
#include <linux/input.h>
#include <linux/hid.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>

#include "nano_macro.h"
#include "nano_input.h"

struct nanosic_input_devices {
	struct hid_device *consumer;
	struct hid_device *keyboard;
	struct hid_device *touch;
	struct hid_device *mouse;
};

static DEFINE_MUTEX(gInputDevicesLock);
static struct nanosic_input_devices __rcu *gInputDevices;

/*定义虚拟化的键盘,鼠标,触摸设备*/
static struct nano_input_create_dev gConsumerDevice =
{
    .name = "Xiaomi Consumer",
    .rd_data = HID_ConsumerReportDescriptor,
    .rd_size = sizeof(HID_ConsumerReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA4,
    .bus     = BUS_VIRTUAL,
};

static struct nano_input_create_dev gKeyboardDevice =
{
    .name = "Xiaomi Keyboard",
    .rd_data = HID_KeyboardReportDescriptor,
    .rd_size = sizeof(HID_KeyboardReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA3,
    .bus     = BUS_VIRTUAL,
};
/*鼠标通道*/
static struct nano_input_create_dev gMouseDevice =
{
    .name = "Xiaomi Mouse",
    .rd_data = HID_MouseReportDescriptor,
    .rd_size = sizeof(HID_MouseReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA2,
    .bus     = BUS_VIRTUAL,
};
/*触摸通道*/
static struct nano_input_create_dev gTouchDevice =
{
    .name = "Xiaomi Touch",
    .rd_data = HID_TouchReportDescriptor,
    .rd_size = sizeof(HID_TouchReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA1,
    .bus     = BUS_VIRTUAL,
};

/** **************************************************************************
 * @brief Nanosic_input_write
 *        data inject to input layer , 键盘,鼠标,多指数据的注入
 ** */
int
Nanosic_input_write(EM_PacketType type ,void *buf, size_t len)
{
	struct nanosic_input_devices *devices;
	struct hid_device *hid = NULL;
	int ret = 0;

	if (!buf || !len)
		return -EINVAL;

	/*
	 * Reports can arrive from both workqueue and timer contexts.  Keep the
	 * published device set alive with RCU while allowing disabled reports to
	 * be dropped without taking a sleeping lock.
	 */
	rcu_read_lock();
	devices = rcu_dereference(gInputDevices);
	if (!devices)
		goto out;

	switch(type)
	{
		case EM_PACKET_KEYBOARD:
			hid = devices->keyboard;
			break;

		case EM_PACKET_CONSUMER:
			hid = devices->consumer;
			break;

		case EM_PACKET_MOUSE:
			hid = devices->mouse;
			break;

		case EM_PACKET_TOUCH:
			hid = devices->touch;
			break;

		default:
			break;
	}

	if (hid) {
		ret = hid_input_report(hid, HID_INPUT_REPORT, buf,
				       min_t(size_t, len, 100), 0);
		if (ret)
			dbgprint(ERROR_LEVEL,
				 "Nanosic_input_write: report type %d err: %d\n",
				 type, ret);
	}

out:
	rcu_read_unlock();
	return ret;
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_start
 *
 *  @brief
 *
 ** */
static int
Nanosic_input_hid_start(struct hid_device *hid)
{
    dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_start ****\n");
    return 0;
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_stop
 *
 *  @brief
 *
 ** */
static void
Nanosic_input_hid_stop(struct hid_device *hid)
{
    hid->claimed = 0;
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_stop ****\n");
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_open
 *
 *  @brief
 *
 ** */
static int
Nanosic_input_hid_open(struct hid_device *hid)
{
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_open ****\n");
    return 0;
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_close
 *
 *  @brief
 *
 ** */
static void
Nanosic_input_hid_close(struct hid_device *hid)
{
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_close ****\n");
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_parse
 *
 *  @brief  OK
 *
 ** */
static int
Nanosic_input_hid_parse(struct hid_device *hid)
{
	struct nano_input_create_dev* ev = hid->driver_data;
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_parse ****\n");
	return hid_parse_report(hid, ev->rd_data, ev->rd_size);
}

/** ************************************************************************//**
 *  @func Nanosic_input_raw_request
 *
 *  @brief support send raw report to hidraw device
 *
 ** */
static int
Nanosic_input_raw_request (struct hid_device *hdev, unsigned char reportnum,
                                        __u8 *buf, size_t len, unsigned char rtype,int reqtype)
{
    return len;
}

/** ************************************************************************//**
 *  @func Nanosic_input_set_report
 *
 *  @brief support set report to i2c slave device
 *
 ** */
static int
Nanosic_input_set_report(struct hid_device *hid, __u8 *buf, size_t size)
{

    if(IS_ERR(hid)){
        dbgprint(ERROR_LEVEL, "Invaild argment\n");
        return -EINVAL;
    }

    if(size <= 0){
        dbgprint(ERROR_LEVEL, "Invaild argment\n");
        return -EINVAL;
    }

    if(IS_ERR(buf)){
        dbgprint(ERROR_LEVEL, "Invaild argment\n");
        return -EINVAL;
    }

    rawdata_show("would not write i2c cmd" ,buf , size);
    return -EINVAL;

    /*return Nanosic_i2c_write(gI2c_client,buf , size);*/
}

/** ************************************************************************//**
 *  @func Nanosic_hid_ll_driver
 *
 *  @brief
 *
 ** */
static struct hid_ll_driver Nanosic_hid_ll_driver = {
	.start = Nanosic_input_hid_start,       /*call on probe dev*/
	.stop  = Nanosic_input_hid_stop,        /*call on remove dev*/
	.open  = Nanosic_input_hid_open,        /*call on input layer open*/
	.close = Nanosic_input_hid_close,       /*call on i input layer close*/
	.parse = Nanosic_input_hid_parse,       /*copy report map description*/
	.raw_request = Nanosic_input_raw_request,
	.output_report = Nanosic_input_set_report,
};

/** ************************************************************************//**
 *  @func Nanosic_input_create
 *
 *  @brief
 *
 ** */
static struct hid_device *
Nanosic_input_create(struct nano_input_create_dev* ev)
{
	struct hid_device *hid = NULL;
	int ret;

	hid = hid_allocate_device();
	if (IS_ERR(hid))
		return hid;
	if (!hid)
		return ERR_PTR(-ENOMEM);

    strncpy(hid->name, ev->name, 127);
    hid->name[127] = 0;
    strncpy(hid->phys, ev->phys, 63);
    hid->phys[63] = 0;
    strncpy(hid->uniq, ev->uniq, 63);
    hid->uniq[63] = 0;

    hid->ll_driver = &Nanosic_hid_ll_driver;
    hid->bus = ev->bus;
    hid->vendor = ev->vendor;
    hid->product = ev->product;
    hid->version = ev->version;
    hid->country = ev->country;
    hid->driver_data = ev;
    hid->dev.parent = NULL;

    ret = hid_add_device(hid);
    if (ret) {
        hid_err(hid, "Cannot register HID device\n");
        goto err_hid;
    }

    return hid;

err_hid:

	hid_destroy_device(hid);
	return ERR_PTR(ret);
}

static void
Nanosic_input_destroy(struct nanosic_input_devices *devices)
{
	if (!devices)
		return;

	if (!IS_ERR_OR_NULL(devices->consumer))
		hid_destroy_device(devices->consumer);
	if (!IS_ERR_OR_NULL(devices->touch))
		hid_destroy_device(devices->touch);
	if (!IS_ERR_OR_NULL(devices->mouse))
		hid_destroy_device(devices->mouse);
	if (!IS_ERR_OR_NULL(devices->keyboard))
		hid_destroy_device(devices->keyboard);

	kfree(devices);
}

/** ************************************************************************//**
 *  @func Nanosic_input_register
 *
 *  @brief register hid virtual device { 注册对应的input 虚拟设备 }
 *
 ** */
int
Nanosic_input_register(void)
{
	struct nanosic_input_devices *devices;
	int ret = 0;

	mutex_lock(&gInputDevicesLock);
	if (rcu_access_pointer(gInputDevices))
		goto out_unlock;

	/* Discard reports from any previous input device instance. */
	Nanosic_cache_clear();

	devices = kzalloc(sizeof(*devices), GFP_KERNEL);
	if (!devices) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	devices->keyboard = Nanosic_input_create(&gKeyboardDevice);
	if (IS_ERR_OR_NULL(devices->keyboard))
		goto err_destroy;

	devices->mouse = Nanosic_input_create(&gMouseDevice);
	if (IS_ERR_OR_NULL(devices->mouse))
		goto err_destroy;

	devices->touch = Nanosic_input_create(&gTouchDevice);
	if (IS_ERR_OR_NULL(devices->touch))
		goto err_destroy;

	devices->consumer = Nanosic_input_create(&gConsumerDevice);
	if (IS_ERR_OR_NULL(devices->consumer))
		goto err_destroy;

	rcu_assign_pointer(gInputDevices, devices);
	dbgprint(DEBUG_LEVEL, "input create ok\n");
	goto out_unlock;

err_destroy:
	if (IS_ERR(devices->consumer))
		ret = PTR_ERR(devices->consumer);
	else if (IS_ERR(devices->touch))
		ret = PTR_ERR(devices->touch);
	else if (IS_ERR(devices->mouse))
		ret = PTR_ERR(devices->mouse);
	else if (IS_ERR(devices->keyboard))
		ret = PTR_ERR(devices->keyboard);
	else
		ret = -ENODEV;

	Nanosic_input_destroy(devices);
	dbgprint(ERROR_LEVEL, "input create err: %d\n", ret);
out_unlock:
	mutex_unlock(&gInputDevicesLock);
	return ret;
}

/** ************************************************************************//**
 *  @func Nanosic_input_release
 *
 *  @brief
 *
 ** */
int
Nanosic_input_release(void)
{
	struct nanosic_input_devices *devices;

	mutex_lock(&gInputDevicesLock);
	devices = rcu_dereference_protected(gInputDevices,
					    lockdep_is_held(&gInputDevicesLock));
	if (!devices) {
		Nanosic_cache_clear();
		goto out_unlock;
	}

	rcu_assign_pointer(gInputDevices, NULL);
	synchronize_rcu();
	Nanosic_cache_clear();
	Nanosic_input_destroy(devices);
	dbgprint(DEBUG_LEVEL,"input release ok\n");

out_unlock:
	mutex_unlock(&gInputDevicesLock);

	return 0;
}

bool
Nanosic_input_enabled(void)
{
	return rcu_access_pointer(gInputDevices) != NULL;
}
