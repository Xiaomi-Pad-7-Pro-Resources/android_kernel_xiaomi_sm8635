/** ***************************************************************************
 * @file nano_timer.c
 *
 * @brief nanosic timer file
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
#include <linux/gpio/consumer.h>
#include "nano_macro.h"

#define PM_TIMER_TIMEOUT_MS 20000

static struct timer_list pm_timer;
static struct work_struct pm_sleep_work;
static bool pm_initialized;

/** ************************************************************************//**
*  @func Nanosic_PM_Sleep
*
*  @brief use to control 803 go to sleep
** */
/** ************************************************************************//**
*  @func Nanosic_PM_try_wakeup
*
*  @brief try to control 803 wakeup from lower power mode
** */
int Nanosic_PM_try_wakeup_locked(void)
{
    int err;
    int level;
    int retry = 3;

    lockdep_assert_held(&g_nanosic_hw_lock);

    if (!pm_initialized)
        return -ENODEV;
    if (!READ_ONCE(g_panel_status))
        return 0;

    /*get gpio_irq_pin's level , high level present 803 in running state*/
    level = Nanosic_GPIO_irq_level_locked();
    if (level < 0)
        return level;

    mod_timer(&pm_timer, jiffies + msecs_to_jiffies(PM_TIMER_TIMEOUT_MS));
    err = Nanosic_GPIO_sleep_locked(true);
    if (err)
        return err;

    if (level == 0) {
        dbgprint(ERROR_LEVEL,"waiting for wakeup...\n");
        mdelay(25);
        while (retry--) {
            /*try three times*/
            level = Nanosic_GPIO_irq_level_locked();
            if (level < 0)
                return level;
            if (level > 0)
                break;

            /*reset wn8030*/
            if (retry == 0) {
                err = Nanosic_GPIO_reset_locked();
                mdelay(1);
                return err;
            }

            /*irq low level duration is 1ms*/
            mdelay(1);
        }
    }

    return 0;
}

int Nanosic_PM_try_wakeup(void)
{
    int err;

    mutex_lock(&g_nanosic_hw_lock);
    err = Nanosic_PM_try_wakeup_locked();
    mutex_unlock(&g_nanosic_hw_lock);

    return err;
}

/** **************************************************************************
* @func  Nanosic_PM_expire
*
* @brief Handler for timer expire
*
** */
static void Nanosic_PM_sleep_work(struct work_struct *work)
{
    int err;

    mutex_lock(&g_nanosic_hw_lock);
    /* New traffic may have rearmed the timer before this work ran. */
    if (pm_initialized && !timer_pending(&pm_timer)) {
        dbgprint(INFO_LEVEL,"going to sleep\n");
        err = Nanosic_GPIO_sleep_locked(false);
        if (err && err != -EHOSTDOWN)
            dbgprint(ERROR_LEVEL,"PM sleep failed: %d\n",err);
    }
    mutex_unlock(&g_nanosic_hw_lock);
}

static void Nanosic_PM_expire(struct timer_list *timer)
{
    schedule_work(&pm_sleep_work);
}

/** ************************************************************************//**
*  @func Nanosic_timer_register
*
*  @brief create a timer
*
** */
int Nanosic_PM_init(void)
{
    int err;

    INIT_WORK(&pm_sleep_work, Nanosic_PM_sleep_work);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,14,0)
     setup_timer(&pm_timer, Nanosic_PM_expire, 0);
#else
     timer_setup(&pm_timer, Nanosic_PM_expire, 0);
#endif
    mutex_lock(&g_nanosic_hw_lock);
    if (!g_nanosic_hw_ready) {
        err = -ENODEV;
        goto out_unlock;
    }

    pm_initialized = true;
    err = Nanosic_PM_try_wakeup_locked();
    if (err)
        pm_initialized = false;

out_unlock:
    mutex_unlock(&g_nanosic_hw_lock);
    if (err) {
        del_timer_sync(&pm_timer);
        cancel_work_sync(&pm_sleep_work);
    }

    if (!err)
        dbgprint(ALERT_LEVEL,"PM module initial\n");
    return err;
}

/** ************************************************************************//**
 *  @func Nanosic_timer_exit
 *
 *  @brief  destroy the timer
 *
 ** */
void Nanosic_PM_free(void)
{
    mutex_lock(&g_nanosic_hw_lock);
    pm_initialized = false;
    mutex_unlock(&g_nanosic_hw_lock);

    del_timer_sync(&pm_timer);
    cancel_work_sync(&pm_sleep_work);

    dbgprint(ALERT_LEVEL,"PM module release\n");
}
