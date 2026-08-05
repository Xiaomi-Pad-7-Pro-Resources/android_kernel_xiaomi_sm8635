/** ***************************************************************************
 * @file nano_driver.c
 *
 * @brief provided interface of initialza and release nanosic driver .
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
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/workqueue.h>
#include <drm/drm_panel.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include "nano_macro.h"

static bool initial = false;
static struct xiaomi_keyboard_data *mdata;
struct nano_i2c_client* gI2c_client=NULL;
static struct drm_panel *active_panel = NULL;

static void keyboard_drm_notifier_callback(enum panel_event_notifier_tag notifier_tag,
                                           struct panel_event_notification *notification,
                                           void *client_data);
static void xiaomi_keyboard_deinit(void);

static int send_screen_state(Screen_StatusType state)
{
    int i = 0, ret = 0;
    struct nano_i2c_client* I2c_client = gI2c_client;
    char cmd[I2C_DATA_LENGTH_WRITE] = {0x32, 0x00, 0x4E, 0x31, 0x80, FIELD_176X, 0x25, 0x01};

    if(IS_ERR_OR_NULL(I2c_client))
        return -ENODEV;

    cmd[SCREEN_STATUS_OFFSET] = (char)state;
    for(i = 2; i < SCREEN_DATA_LENGTH; i++)
        cmd[SCREEN_DATA_LENGTH] += cmd[i]; /*cal sum*/
    rawdata_show("write screen state cmd", cmd, sizeof(cmd));
    ret = Nanosic_i2c_write(I2c_client, cmd, sizeof(cmd));

    return ret;
}

static int nanosic_set_panel_power(bool panel_on, bool awake)
{
    int ret;

    mutex_lock(&g_nanosic_hw_lock);
    WRITE_ONCE(g_panel_status, panel_on);
    ret = Nanosic_GPIO_sleep_locked(awake);
    mutex_unlock(&g_nanosic_hw_lock);

    return ret;
}

static int keyboard_check_panel(struct device_node *np)
{
    int i;
    int count;
    struct device_node *node;
    struct drm_panel *panel;
    int err = -ENODEV;

    count = of_count_phandle_with_args(np, "panel", NULL);
    dbgprint(ALERT_LEVEL, "keyboard_check_panel panel count=%d\n", count);
    if (count <= 0)
        return -ENODEV;

    for (i = 0; i < count; i++)
    {
        node = of_parse_phandle(np, "panel", i);
        panel = of_drm_find_panel(node);
        if (!IS_ERR(panel))
        {
            active_panel = panel;
            if (active_panel)
                dbgprint(ALERT_LEVEL, "keyboard_check_panel check panel ok\n");
            of_node_put(node);
            return 0;
        }
        err = PTR_ERR(panel);
        of_node_put(node);
        active_panel = NULL;
    }

    if (err == -EPROBE_DEFER)
        return err;

    return -ENODEV;
}

static int nanosic_803_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret=0;
    struct device_node* of_node;
    int irq_pin=-1;
    u32 irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
    int reset_pin=-1;
    int status_pin=-1;
    int vdd_pin=-1;
    int sleep_pin=-1;
    struct nano_i2c_client* I2client=NULL;

    if (IS_ERR_OR_NULL(client) || !client->adapter) {
        dbgprint(ERROR_LEVEL,"nanosic_803_probe client IS_ERR_OR_NULL\n");
        return -EINVAL;
    }

    dbgprint(ALERT_LEVEL,"probe adapter nr %d, addr 0x%x\n",client->adapter->nr,client->addr);

    of_node = client->dev.of_node;
    if(!of_node) {
        dbgprint(ERROR_LEVEL,"nanosic_803_probe of_node == 0\n");
        return -EINVAL;
    }

    ret = keyboard_check_panel(of_node);
    if(ret < 0) {
        if(ret == -EPROBE_DEFER) {
            dbgprint(ERROR_LEVEL,"nanosic_803_probe, panel not ready, wait retry!!!\n");
            return -EPROBE_DEFER;
        }
        dbgprint(ERROR_LEVEL,"nanosic_803_probe ret=%d\n", ret);
        return -ENODEV;
    }

    irq_pin = of_get_named_gpio_flags(of_node, "irq_pin", 0,
                                      (enum of_gpio_flags *)&irq_flags);
    dbgprint(ALERT_LEVEL,"irq_pin=%d,irq_flags=%d\n", irq_pin,irq_flags);
    reset_pin = of_get_named_gpio_flags(of_node, "reset_pin", 0, NULL);
    dbgprint(ALERT_LEVEL,"reset_pin=%d\n", reset_pin);
    status_pin = of_get_named_gpio_flags(of_node, "status_pin", 0, NULL);
    dbgprint(ALERT_LEVEL,"status_pin=%d\n", status_pin);
    vdd_pin = of_get_named_gpio_flags(of_node, "vdd_pin", 0, NULL);
    dbgprint(ALERT_LEVEL,"vdd_pin=%d\n", vdd_pin);
    sleep_pin = of_get_named_gpio_flags(of_node, "sleep_pin", 0, NULL);
    dbgprint(ALERT_LEVEL,"sleep_pin=%d\n", sleep_pin);
    gpio_hall_n_pin = of_get_named_gpio_flags(of_node, "hall_n_pin", 0, NULL);
    gpio_hall_s_pin = of_get_named_gpio_flags(of_node, "hall_s_pin", 0, NULL);
    dbgprint(ALERT_LEVEL,"hall_n_pin=%d,hall_s_pin=%d\n",
             gpio_hall_n_pin, gpio_hall_s_pin);

    if (irq_pin == -EPROBE_DEFER || reset_pin == -EPROBE_DEFER ||
        status_pin == -EPROBE_DEFER || vdd_pin == -EPROBE_DEFER ||
        sleep_pin == -EPROBE_DEFER)
        return -EPROBE_DEFER;
    if (!gpio_is_valid(irq_pin) || !gpio_is_valid(reset_pin) ||
        !gpio_is_valid(status_pin) || !gpio_is_valid(vdd_pin) ||
        !gpio_is_valid(sleep_pin))
        return -EINVAL;

    ret = Nanosic_GPIO_register(vdd_pin,reset_pin,status_pin,irq_pin,sleep_pin);
    if(ret < 0) {
        dbgprint(ERROR_LEVEL,"GPIO register ERROR!\n\n");
        goto _err1;
    }

    /*initialize 8030 pm*/
    ret = Nanosic_PM_init();
    if (ret)
        goto _err_gpio;

    /* Status reports may arrive as soon as the I2C IRQ is installed. */
    ret = Nanosic_cache_init();
    if (ret)
        goto _err_pm;

    WRITE_ONCE(gHallStatus, 0);

    /*initialize i2c module*/
    mutex_lock(&g_nanosic_hw_lock);
    I2client = Nanosic_i2c_register(irq_pin,irq_flags,client->adapter->nr,client->addr);
    if (IS_ERR(I2client)) {
        ret = PTR_ERR(I2client);
        I2client = NULL;
    } else {
        I2client->dev = &client->dev;
        gI2c_client = I2client;
        i2c_set_clientdata(client,I2client);
    }
    mutex_unlock(&g_nanosic_hw_lock);
    if (!I2client)
        goto _err_cache;

    /*i2c device can wakeup system*/
    ret = device_init_wakeup(&client->dev, true);
    if (ret)
        goto _err_i2c;

    ret = xiaomi_keyboard_init(I2client);
    if (ret < 0)
        goto _err_wakeup;

    /* The HID devices are created only by a valid attached status report. */
    ret = Nanosic_chardev_register();
    if(ret < 0)
        goto _err_keyboard;

    initial = true;
    ret = Nanosic_i2c_request_status(I2client);
    if (ret != I2C_DATA_LENGTH_WRITE)
        dbgprint(ERROR_LEVEL, "initial keyboard status query failed: %d\n", ret);
    dbgprint(ALERT_LEVEL,"probe nanosic driver\n");

    return 0;

_err_keyboard:
    xiaomi_keyboard_deinit();
_err_wakeup:
    device_init_wakeup(&client->dev, false);
_err_i2c:
    Nanosic_GPIO_irq_release();
    mutex_lock(&g_nanosic_hw_lock);
    gI2c_client = NULL;
    i2c_set_clientdata(client, NULL);
    mutex_unlock(&g_nanosic_hw_lock);
    Nanosic_i2c_release(I2client);
    /* The IRQ workqueue is drained, so it cannot recreate a HID device. */
    Nanosic_input_release();
_err_cache:
    Nanosic_cache_release();
_err_pm:
    Nanosic_PM_free();
_err_gpio:
    Nanosic_GPIO_release();
    return ret < 0 ? ret : -ENODEV;
_err1:
    return ret < 0 ? ret : -ENODEV;
}

static int nanosic_803_remove(struct i2c_client *client)
{
    struct nano_i2c_client* I2client=NULL;

    dbgprint(ALERT_LEVEL,"remove\n");
    if(initial == false)
        return 0;
    initial = false;

    I2client = i2c_get_clientdata(client);

    /*release chardev module*/
    Nanosic_chardev_release();

    /* Exclude recovery while invalidating and freeing its client storage. */
    mutex_lock(&g_nanosic_lifecycle_lock);

    /*stop panel callbacks and drain their work before releasing PM/I2C state*/
    xiaomi_keyboard_deinit();

    device_init_wakeup(&client->dev, false);

    Nanosic_PM_free();

    /* Synchronize the status IRQ before invalidating its I2C/input target. */
    Nanosic_GPIO_irq_release();

    /* Stop publishing the I2C client before draining IRQ/workqueue users. */
    mutex_lock(&g_nanosic_hw_lock);
    gI2c_client = NULL;
    i2c_set_clientdata(client, NULL);
    mutex_unlock(&g_nanosic_hw_lock);

    /*release the main I2C IRQ/workqueue before freeing its GPIO pin*/
    Nanosic_i2c_release(I2client);
    /* The drained workqueue can no longer recreate an attached HID device. */
    Nanosic_input_release();
    Nanosic_GPIO_release();

    Nanosic_cache_release();

    mutex_unlock(&g_nanosic_lifecycle_lock);

    dbgprint(ALERT_LEVEL,"remove nanosic driver\n");

    return 0;
}

static const struct of_device_id nanosic_803_of_match[] = {
	{.compatible = "nanosic,803",},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nanosic_803_of_match);

static const struct i2c_device_id nanosic_803_i2c_id[] = {
    { "nanosic,803", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, nanosic_803_i2c_id);

static int xiaomi_keyboard_init(struct nano_i2c_client* i2c_client)
{
    int ret = 0;
    dbgprint(ALERT_LEVEL,"xiaomi_keyboard_init: enter\n");
    mdata = kzalloc(sizeof(struct xiaomi_keyboard_data), GFP_KERNEL);
    if (!mdata)
        return -ENOMEM;

    mdata->dev_pm_suspend = false;
    mdata->irq = i2c_client->irqno;
    dbgprint(ALERT_LEVEL,"xiaomi_keyboard_init:irq:%d",mdata->irq);
    mdata->event_wq = create_singlethread_workqueue("kb-event-queue");
    if (!mdata->event_wq) {
        dbgprint(ALERT_LEVEL,"xiaomi_keyboard_init:Can not create work thread for suspend/resume!!");
        ret = -ENOMEM;
        goto err_free;
    }
    INIT_WORK(&mdata->early_resume_work, keyboard_early_resume_work);
    INIT_WORK(&mdata->resume_work, keyboard_resume_work);
    INIT_WORK(&mdata->early_suspend_work, keyboard_early_suspend_work);
    INIT_WORK(&mdata->suspend_work, keyboard_suspend_work);

    if (active_panel) {
        mdata->notifier_cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
                                                               PANEL_EVENT_NOTIFIER_CLIENT_KEYBOARD,
                                                               active_panel,
                                                               keyboard_drm_notifier_callback,
                                                               (void *)mdata);
        if (!mdata->notifier_cookie) {
            dbgprint(ALERT_LEVEL,"xiaomi_keyboard_init:register drm_notifier failed. cookie=%s\n",
                     (char *)mdata->notifier_cookie);
            ret = -ENODEV;
            goto err_destroy_workqueue;
        }
    }
    dbgprint(ALERT_LEVEL,"xiaomi_keyboard_init: success. \n");
    return ret;

err_destroy_workqueue:
    destroy_workqueue(mdata->event_wq);
err_free:
    kfree(mdata);
    mdata = NULL;
    return ret;
}

static int xiaomi_keyboard_disable_irq_wake_locked(
        struct xiaomi_keyboard_data *keyboard_data)
{
    int first_err = 0;
    int ret;

    lockdep_assert_held(&g_nanosic_hw_lock);

    if (keyboard_data->status_irq_wake_enabled) {
        ret = disable_irq_wake(g_wakeup_irqno);
        if (!ret)
            keyboard_data->status_irq_wake_enabled = false;
        else
            first_err = ret;
    }

    if (keyboard_data->main_irq_wake_enabled) {
        ret = disable_irq_wake(keyboard_data->irq);
        if (!ret)
            keyboard_data->main_irq_wake_enabled = false;
        else if (!first_err)
            first_err = ret;
    }

    return first_err;
}

static void xiaomi_keyboard_deinit(void)
{
    if (!mdata)
        return;

    if (mdata->notifier_cookie) {
        panel_event_notifier_unregister(mdata->notifier_cookie);
        mdata->notifier_cookie = NULL;
    }

    if (mdata->event_wq) {
        destroy_workqueue(mdata->event_wq);
        mdata->event_wq = NULL;
    }

    mutex_lock(&g_nanosic_hw_lock);
    if (xiaomi_keyboard_disable_irq_wake_locked(mdata))
        dbgprint(ERROR_LEVEL,"failed to disable an IRQ wake source\n");
    mdata->dev_pm_suspend = false;
    g_nanosic_hw_suspended = false;
    mutex_unlock(&g_nanosic_hw_lock);

    kfree(mdata);
    mdata = NULL;
}

static void keyboard_early_resume_work(struct work_struct *work)
{
    int ret = 0;

    (void)work;
    dbgprint(ALERT_LEVEL,"keyboard_early_resume_work: enter\n");
    ret = Nanosic_cache_put();
    if(ret < 0)
        dbgprint(ERROR_LEVEL,"keyboard_early_resume_work: Nanosic_cache_put err:%d\n",ret);
    ret = nanosic_set_panel_power(true, true);
    if (ret < 0)
        dbgprint(ERROR_LEVEL,"keyboard_early_resume_work: wake err:%d\n",ret);
    if (gI2c_client)
        Nanosic_workQueue_schedule(gI2c_client->worker);
    ret = Nanosic_RequestGensor_notify();
    if(ret < 0)
        dbgprint(ERROR_LEVEL,"keyboard_early_resume_work: Nanosic_RequestGensor_notify err:%d\n",ret);
    ret = Nanosic_Hall_notify(gpio_hall_n_pin, gpio_hall_s_pin);
    if(ret < 0)
        dbgprint(ERROR_LEVEL,"keyboard_early_resume_work: Nanosic_Hall_notify err:%d\n",ret);
}

static void keyboard_resume_work(struct work_struct *work)
{
    int ret = 0;

    (void)work;
    dbgprint(ALERT_LEVEL,"keyboard_resume_work: enter\n");
    ret = send_screen_state(SCREEN_ON);
    dbgprint(ALERT_LEVEL,"keyboard_resume_work: send screen on:%d\n", ret);
}

static void keyboard_early_suspend_work(struct work_struct *work)
{
    int ret = 0;

    (void)work;
    dbgprint(ALERT_LEVEL,"keyboard_early_suspend_work: enter\n");
    ret = send_screen_state(SCREEN_OFF);
    dbgprint(ALERT_LEVEL,"keyboard_early_suspend_work: send screen off:%d\n", ret);
}

static void keyboard_suspend_work(struct work_struct *work)
{
    int ret;

    (void)work;
    dbgprint(ALERT_LEVEL,"keyboard_suspend_work: enter\n");
    ret = nanosic_set_panel_power(false, false);
    if (ret < 0)
        dbgprint(ERROR_LEVEL,"keyboard_suspend_work: sleep err:%d\n",ret);
}

static int xiaomi_keyboard_pm_suspend(struct device *dev)
{
    struct nano_i2c_client *i2c_client;
    int ret;

    (void)dev;
    if (!mdata)
        return -ENODEV;

    dbgprint(ALERT_LEVEL,"xiaomi_keyboard_pm_suspend: enter, enable_irq_wake\n");

    mutex_lock(&g_nanosic_hw_lock);
    if (mdata->dev_pm_suspend) {
        ret = 0;
        goto out_unlock;
    }
    if (!g_nanosic_hw_ready || g_nanosic_hw_recovering) {
        ret = -EBUSY;
        goto out_unlock;
    }
    if (g_nanosic_hw_suspended) {
        ret = -EALREADY;
        goto out_unlock;
    }

    ret = xiaomi_keyboard_disable_irq_wake_locked(mdata);
    if (ret)
        goto out_unlock;

    i2c_client = gI2c_client;
    if (!i2c_client || !i2c_client->irq_registered ||
        !Nanosic_GPIO_irq_registered_locked()) {
        ret = -ENXIO;
        goto out_unlock;
    }

    ret = enable_irq_wake(mdata->irq);
    if (ret)
        goto out_unlock;
    mdata->main_irq_wake_enabled = true;

    ret = enable_irq_wake(g_wakeup_irqno);
    if (ret) {
        int rollback_ret;

        rollback_ret = xiaomi_keyboard_disable_irq_wake_locked(mdata);
        if (rollback_ret)
            dbgprint(ERROR_LEVEL,"failed to roll back IRQ wake: %d\n",
                     rollback_ret);
        goto out_unlock;
    }
    mdata->status_irq_wake_enabled = true;

    g_nanosic_hw_suspended = true;
    mdata->dev_pm_suspend = true;
    ret = 0;

out_unlock:
    mutex_unlock(&g_nanosic_hw_lock);
    return ret;
}

static int xiaomi_keyboard_pm_resume(struct device *dev)
{
    int ret;

    (void)dev;
    if (!mdata)
        return 0;

    dbgprint(ALERT_LEVEL,"xiaomi_keyboard_pm_resume enter, disable_irq_wake\n");

    mutex_lock(&g_nanosic_hw_lock);
    ret = xiaomi_keyboard_disable_irq_wake_locked(mdata);
    g_nanosic_hw_suspended = false;
    mdata->dev_pm_suspend = false;
    /* Complete an IRQ read deferred while the I2C controller was suspended. */
    if (gI2c_client && gI2c_client->read_pending) {
        gI2c_client->read_pending = false;
        Nanosic_workQueue_schedule(gI2c_client->worker);
    }
    mutex_unlock(&g_nanosic_hw_lock);

    if (ret)
        dbgprint(ERROR_LEVEL,"xiaomi_keyboard_pm_resume: disable IRQ wake failed: %d\n",ret);
    return ret;
}

static const struct dev_pm_ops xiaomi_keyboard_pm_ops = {
    .suspend = xiaomi_keyboard_pm_suspend,
    .resume = xiaomi_keyboard_pm_resume,
};

static void keyboard_drm_notifier_callback(enum panel_event_notifier_tag notifier_tag,
                                           struct panel_event_notification *notification,
                                           void *client_data)
{
    struct xiaomi_keyboard_data *mclient_data = (struct xiaomi_keyboard_data *)client_data;

    (void)notifier_tag;
    if (!notification || !mclient_data || !mclient_data->event_wq)
    {
        dbgprint(ERROR_LEVEL, "Invalid notification\n");
        return;
    }

    switch (notification->notif_type)
    {
    case DRM_PANEL_EVENT_BLANK:
    case DRM_PANEL_EVENT_BLANK_LP:
        if (notification->notif_data.early_trigger)
        {
            queue_work(mclient_data->event_wq, &mclient_data->early_suspend_work);
            dbgprint(ALERT_LEVEL, "keyboard_drm_notifier_callback early_trigger keyboard suspend\n");
        }
        else
        {
            queue_work(mclient_data->event_wq, &mclient_data->suspend_work);
            dbgprint(ALERT_LEVEL, "keyboard_drm_notifier_callback keyboard suspend\n");
        }
        break;
    case DRM_PANEL_EVENT_UNBLANK:
        if (notification->notif_data.early_trigger)
        {
            queue_work(mclient_data->event_wq, &mclient_data->early_resume_work);
            dbgprint(ALERT_LEVEL, "keyboard_drm_notifier_callback early_trigger keyboard resume\n");
        }
        else
        {
            queue_work(mclient_data->event_wq, &mclient_data->resume_work);
            dbgprint(ALERT_LEVEL, "keyboard_drm_notifier_callback keyboard resume\n");
        }
        break;
    default:
        dbgprint(DEBUG_LEVEL, "keyboard_drm_notifier_callback notif_type=%d\n", notification->notif_type);
        break;
    }
}

static struct i2c_driver nanosic_803_driver = {
	.probe		= nanosic_803_probe,
	.remove		= nanosic_803_remove,
	.driver = {
		.name	= "nanosic,803",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(nanosic_803_of_match),
        .pm = &xiaomi_keyboard_pm_ops,
	},
    .id_table       = nanosic_803_i2c_id,
	.detect         = Nanosic_i2c_detect,
};

static __init
int nanosic_driver_init(void)
{
    int ret;

    ret = i2c_add_driver(&nanosic_803_driver);

    return ret;
}

static __exit
void nanosic_driver_exit(void)
{
    i2c_del_driver(&nanosic_803_driver);
}

module_init(nanosic_driver_init);
module_exit(nanosic_driver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("bin.yuan@nanosic.com");
