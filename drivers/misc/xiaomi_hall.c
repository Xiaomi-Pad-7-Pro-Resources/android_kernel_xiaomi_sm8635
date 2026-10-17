// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi hall sensor driver.
 *
 * Reports the lid and tablet-mode switches of a 2-in-1 through an input
 * device.  The open/close interrupts normally come from the hall algorithm
 * running on the ADSP and are delivered over SMP2P; when that algorithm is
 * not available the driver can fall back to watching the raw GPIO and can
 * be switched between the two modes from sysfs.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/uaccess.h>

#define XIAOMI_HALL_NAME		"xiaomi_hall"
#define XIAOMI_HALL_COMPATIBLE		"xiaomi-hall"
#define XIAOMI_HALL_IRQ_NAME_LEN	32
#define XIAOMI_HALL_GPIO_IRQ_NAME	"8030_io_wakeup"

#define hall_print(level, fmt, ...)					\
do {									\
	struct timespec64 ts;						\
	struct rtc_time tm;						\
									\
	ktime_get_real_ts64(&ts);					\
	rtc_time64_to_tm(ts.tv_sec - sys_tz.tz_minuteswest * 60, &tm);	\
	pr_##level("[%02d:%02d:%02d.%03zu] " "[xiaomi-hall]: " fmt,	\
		   tm.tm_hour, tm.tm_min, tm.tm_sec,			\
		   (size_t)(ts.tv_nsec / 1000000), ##__VA_ARGS__);	\
} while (0)

#define hall_info(fmt, ...)	hall_print(info, fmt, ##__VA_ARGS__)
#define hall_err(fmt, ...)	hall_print(err, fmt, ##__VA_ARGS__)

struct xiaomi_hall_data {
	const char *label;
	int hall_pin;
	int gpio_irq;
	u32 code;
	u32 type;
	u32 flags;
	int open_irq;
	int close_irq;
	char *open_irq_name;
	char *close_irq_name;
	struct input_dev *input;
};

struct xiaomi_hall_drv_data {
	int hall_algo_disable;
	int hall_count;
	struct xiaomi_hall_data *hall_data;
	struct input_dev *input;
};

struct xiaomi_hall_cdev {
	struct cdev cdev;
	struct device *device;
	struct class *class;
	dev_t devt;
};

static struct xiaomi_hall_cdev xiaomi_hall_dev;

static int xiaomi_hall_open(struct input_dev *input);
static void xiaomi_hall_close(struct input_dev *input);

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	struct xiaomi_hall_data *hall_data = dev_id;
	int value;

	hall_info("handle irq=%d", irq);

	if (hall_data->open_irq == irq) {
		value = (hall_data->flags ^ 0xffffffff) & 1;
		input_event(hall_data->input, hall_data->type, hall_data->code, value);
		hall_info("input type=%d, code=%d, irq=%d, val=%d,\n",
			  hall_data->type, hall_data->code, hall_data->open_irq, value);
	} else if (hall_data->close_irq == irq) {
		value = hall_data->flags & 1;
		input_event(hall_data->input, hall_data->type, hall_data->code, value);
		hall_info("input type=%d, code=%d, irq=%d, val=%d\n",
			  hall_data->type, hall_data->code, hall_data->close_irq, value);
	} else {
		hall_err("unknow irq=%d\n", irq);
	}

	input_event(hall_data->input, EV_SYN, SYN_REPORT, 0);

	return IRQ_HANDLED;
}

static irqreturn_t lid_hall_gpio_irq_handler(int irq, void *dev_id)
{
	struct xiaomi_hall_data *hall_data = dev_id;
	int value;

	value = gpiod_get_raw_value(gpio_to_desc(hall_data->hall_pin));
	if (hall_data->flags & 1)
		value = value == 0;

	input_event(hall_data->input, hall_data->type, hall_data->code, value);
	hall_info("lid_hall_gpio_irq_handler, input type=%d, code=%d, val=%d\n",
		  hall_data->type, hall_data->code, value);
	input_event(hall_data->input, EV_SYN, SYN_REPORT, 0);

	return IRQ_HANDLED;
}

static int xiaomi_hall_open(struct input_dev *input)
{
	struct xiaomi_hall_drv_data *drv_data = input_get_drvdata(input);
	struct xiaomi_hall_data *hall_data;
	int i, value;

	for (i = 0; i < drv_data->hall_count; i++) {
		hall_data = &drv_data->hall_data[i];

		value = gpiod_get_raw_value(gpio_to_desc(hall_data->hall_pin));
		if (hall_data->flags & 1)
			value = value == 0;

		hall_info("xiaomi_hall_open, %s value=%d", hall_data->label, value);

		input_event(input, hall_data->type, hall_data->code, value);
		input_event(input, EV_SYN, SYN_REPORT, 0);
	}

	return 0;
}

static void xiaomi_hall_close(struct input_dev *input)
{
	struct xiaomi_hall_drv_data *drv_data = input_get_drvdata(input);

	hall_info("xiaomi_hall_close, hall_count=%d", drv_data->hall_count);
}

static ssize_t xiaomi_hall_fops_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct xiaomi_hall_cdev *hall_cdev = file->private_data;
	struct xiaomi_hall_drv_data *drv_data = dev_get_drvdata(hall_cdev->device);
	struct xiaomi_hall_data *hall_data;
	struct input_event event = { 0 };
	int i, value, ret = 0;
	bool found = false;

	for (i = 0; i < drv_data->hall_count; i++) {
		hall_data = &drv_data->hall_data[i];

		if (strcmp("lid_hall", hall_data->label))
			continue;

		if (hall_data->hall_pin < 0) {
			hall_err("xiaomi_hall_fops_read, gpio number < 0");
			continue;
		}

		value = gpiod_get_raw_value(gpio_to_desc(hall_data->hall_pin));
		if (hall_data->flags & 1)
			value = value == 0;

		event.type = hall_data->type;
		event.code = hall_data->code;
		event.value = value;

		hall_info("xiaomi_hall_fops_read, tyep=%d,code=%d,value=%d",
			  event.type, event.code, event.value);

		if (count < sizeof(event)) {
			hall_err("xiaomi_hall_fops_read, buf size < sizeof(input event)");
			break;
		}

		if (copy_to_user(buf, &event, sizeof(event))) {
			hall_err("xiaomi_hall_fops_read, copy_to_user fail.");
			break;
		}

		ret = sizeof(event);
		found = true;
		break;
	}

	if (!found) {
		hall_err("xiaomi_hall_fops_read, not find lid hall gpio");
		ret = -1;
	}

	return ret;
}

static ssize_t xiaomi_hall_fops_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	return -ENOSYS;
}

static int xiaomi_hall_fops_open(struct inode *inode, struct file *file)
{
	file->private_data = &xiaomi_hall_dev;

	hall_info("xiaomi_hall_fops_open.");

	return 0;
}

static int xiaomi_hall_fops_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;

	hall_info("xiaomi_hall_fops_release.");

	return 0;
}

static const struct file_operations xiaomi_hall_fops = {
	.owner = THIS_MODULE,
	.read = xiaomi_hall_fops_read,
	.write = xiaomi_hall_fops_write,
	.open = xiaomi_hall_fops_open,
	.release = xiaomi_hall_fops_release,
};

static ssize_t disable_algo_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_hall_drv_data *drv_data = dev_get_drvdata(dev);

	if (drv_data->hall_algo_disable)
		return sprintf(buf, "hall algo is disable.\n");

	return sprintf(buf, "hall algo is enable.\n");
}

static ssize_t disable_algo_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct xiaomi_hall_drv_data *drv_data = dev_get_drvdata(dev);
	struct xiaomi_hall_data *hall_data;
	int i, value, ret;
	bool found = false;

	ret = kstrtoint(buf, 16, &value);
	if (ret) {
		hall_err("disable_algo_store, kstrtoint fail, err=%d.", ret);
		return 0;
	}

	for (i = 0; i < drv_data->hall_count; i++) {
		hall_data = &drv_data->hall_data[i];

		if (strcmp("lid_hall", hall_data->label))
			continue;

		if (value == 0) {
			if (hall_data->open_irq > 0 && hall_data->close_irq > 0) {
				enable_irq(hall_data->open_irq);
				enable_irq(hall_data->close_irq);
			}

			if (hall_data->hall_pin > 0 && hall_data->gpio_irq) {
				irq_set_irq_wake(hall_data->gpio_irq, 0);
				devm_free_irq(dev, hall_data->gpio_irq, hall_data);
				hall_data->gpio_irq = 0;
			}

			drv_data->hall_algo_disable = 0;
			hall_info("disable_algo_store, xiaomi_hall changes to hall algo mode.");
			found = true;
		} else if (value == 1) {
			if (hall_data->hall_pin > 0) {
				ret = devm_gpio_request(dev, hall_data->hall_pin, NULL);
				if (ret) {
					hall_err("disable_algo_store, gpio_request fail, err=%d.", ret);
					return 0;
				}

				hall_data->gpio_irq =
					gpiod_to_irq(gpio_to_desc(hall_data->hall_pin));

				ret = devm_request_threaded_irq(dev, hall_data->gpio_irq,
								NULL,
								lid_hall_gpio_irq_handler,
								IRQF_ONESHOT |
								IRQF_TRIGGER_RISING |
								IRQF_TRIGGER_FALLING,
								XIAOMI_HALL_GPIO_IRQ_NAME,
								hall_data);
				if (ret < 0) {
					hall_err("disable_algo_store, devm_request_threaded_irq fail, err=%d.",
						 ret);
					return 0;
				}

				irq_set_irq_wake(hall_data->gpio_irq, 1);
			}

			if (hall_data->open_irq > 0 && hall_data->close_irq > 0) {
				disable_irq(hall_data->open_irq);
				disable_irq(hall_data->close_irq);
			} else {
				hall_err("disable_algo_store, invalid open irq or close irq.");
			}

			drv_data->hall_algo_disable = 1;
			hall_info("disable_algo_store, xiaomi_hall changes to gpio mode.");
			found = true;
		} else {
			hall_err("disable_algo_store, unknown command.");
			return 0;
		}
	}

	if (!found) {
		hall_err("disable_algo_store, lid hall not found.");
		return 0;
	}

	return count;
}

static DEVICE_ATTR_RW(disable_algo);

static struct attribute *xiaomi_hall_attrs[] = {
	&dev_attr_disable_algo.attr,
	NULL,
};

static const struct attribute_group xiaomi_hall_attrs_group = {
	.attrs = xiaomi_hall_attrs,
};

static int xiaomi_hall_chardev_init(struct xiaomi_hall_drv_data *drv_data)
{
	int ret;

	ret = alloc_chrdev_region(&xiaomi_hall_dev.devt, 0, 1, "hall");
	if (ret < 0) {
		hall_err("alloc_chrdev_region fail, err=%d.", ret);
		return ret;
	}

	cdev_init(&xiaomi_hall_dev.cdev, &xiaomi_hall_fops);

	ret = cdev_add(&xiaomi_hall_dev.cdev, xiaomi_hall_dev.devt, 1);
	if (ret < 0) {
		hall_err("cdev add fail, err=%d.", ret);
		goto err_unregister_region;
	}

	xiaomi_hall_dev.class = class_create(THIS_MODULE, "hall");
	if (IS_ERR(xiaomi_hall_dev.class)) {
		ret = PTR_ERR(xiaomi_hall_dev.class);
		hall_err("class_create fail, err=%d.", ret);
		goto err_cdev_del;
	}

	xiaomi_hall_dev.device = device_create(xiaomi_hall_dev.class, NULL,
					       MKDEV(MAJOR(xiaomi_hall_dev.devt), 0),
					       drv_data, XIAOMI_HALL_NAME);
	if (IS_ERR(xiaomi_hall_dev.device)) {
		ret = PTR_ERR(xiaomi_hall_dev.device);
		hall_err("device_create fail, err=%d.", ret);
		goto err_class_destroy;
	}

	ret = sysfs_create_group(&xiaomi_hall_dev.device->kobj,
				 &xiaomi_hall_attrs_group);
	if (ret < 0) {
		hall_err("sysfs_create_group fail, err=%d.", ret);
		goto err_device_destroy;
	}

	return 0;

err_device_destroy:
	device_destroy(xiaomi_hall_dev.class, MKDEV(MAJOR(xiaomi_hall_dev.devt), 0));
err_class_destroy:
	class_destroy(xiaomi_hall_dev.class);
err_cdev_del:
	cdev_del(&xiaomi_hall_dev.cdev);
err_unregister_region:
	unregister_chrdev_region(xiaomi_hall_dev.devt, 1);

	return ret;
}

static int xiaomi_hall_parse_dt(struct device *dev,
				struct xiaomi_hall_drv_data *drv_data)
{
	struct fwnode_handle *child;
	struct xiaomi_hall_data *hall_data;
	int i = 0, ret;

	device_for_each_child_node(dev, child) {
		hall_data = &drv_data->hall_data[i];

		ret = fwnode_property_read_string(child, "label", &hall_data->label);
		if (ret) {
			hall_err("cant read node%d lable, err = %d.", i + 1, ret);
			goto err;
		}
		hall_info("node%d lable = %s.", i + 1, hall_data->label);

		ret = fwnode_property_read_u32_array(child, "linux,code",
						     &hall_data->code, 1);
		if (ret) {
			hall_err("cant read node%d code, err = %d.", i + 1, ret);
			goto err;
		}
		hall_info("node%d linux,code = %u.", i + 1, hall_data->code);

		ret = fwnode_property_read_u32_array(child, "linux,input-type",
						     &hall_data->type, 1);
		if (ret) {
			hall_err("cant read node%d type, err = %d.", i + 1, ret);
			goto err;
		}
		hall_info("node%d linux,type = %u.", i + 1, hall_data->type);

		hall_data->hall_pin = of_get_named_gpio_flags(to_of_node(child),
							      "hall_pin", 0,
							      (enum of_gpio_flags *)&hall_data->flags);

		hall_data->open_irq = of_irq_get_byname(to_of_node(child), "open_irq");
		if (hall_data->open_irq < 0) {
			hall_err("hall_data->open_irq is %d", hall_data->open_irq);
			hall_err("failed to get open_irq.");
			ret = -EPROBE_DEFER;
			goto err;
		}

		hall_data->close_irq = of_irq_get_byname(to_of_node(child), "close_irq");
		if (hall_data->close_irq < 0) {
			hall_err("failed to get close_irq.");
			ret = -EPROBE_DEFER;
			goto err;
		}

		hall_info("node%d %s_pin=%d, open_irq=%d, close_irq=%d, flags=%u",
			  i + 1, hall_data->label, hall_data->hall_pin,
			  hall_data->open_irq, hall_data->close_irq,
			  hall_data->flags);

		i++;
	}

	return 0;

err:
	return ret;
}

static int xiaomi_hall_register_irqs(struct device *dev,
				     struct xiaomi_hall_drv_data *drv_data)
{
	struct xiaomi_hall_data *hall_data;
	int i, ret;

	for (i = 0; i < drv_data->hall_count; i++) {
		hall_data = &drv_data->hall_data[i];

		hall_data->open_irq_name = devm_kmalloc(dev, XIAOMI_HALL_IRQ_NAME_LEN,
							GFP_KERNEL | __GFP_ZERO);
		if (!hall_data->open_irq_name) {
			hall_err("no memory for open_irq_name.");
			return -ENOMEM;
		}
		snprintf(hall_data->open_irq_name, XIAOMI_HALL_IRQ_NAME_LEN,
			 "%s_%s", hall_data->label, "open");
		hall_info("irq name=%s.", hall_data->open_irq_name);

		ret = devm_request_threaded_irq(dev, hall_data->open_irq, NULL,
						irq_handler,
						IRQF_ONESHOT | IRQF_TRIGGER_RISING,
						hall_data->open_irq_name, hall_data);
		if (ret) {
			hall_err("register %s fail, err=%d.",
				 hall_data->open_irq_name, ret);
			return ret;
		}

		hall_data->close_irq_name = devm_kmalloc(dev, XIAOMI_HALL_IRQ_NAME_LEN,
							 GFP_KERNEL | __GFP_ZERO);
		if (!hall_data->close_irq_name) {
			hall_err("no memory for close_irq_name.");
			return -ENOMEM;
		}
		snprintf(hall_data->close_irq_name, XIAOMI_HALL_IRQ_NAME_LEN,
			 "%s_%s", hall_data->label, "close");
		hall_info("irq name=%s.", hall_data->close_irq_name);

		ret = devm_request_threaded_irq(dev, hall_data->close_irq, NULL,
						irq_handler,
						IRQF_ONESHOT | IRQF_TRIGGER_RISING,
						hall_data->close_irq_name, hall_data);
		if (ret) {
			hall_err("register %s fail, err=%d.",
				 hall_data->close_irq_name, ret);
			return ret;
		}
	}

	hall_info("register xiaomi hall irq done.");

	return 0;
}

static int xiaomi_hall_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xiaomi_hall_drv_data *drv_data;
	struct xiaomi_hall_data *hall_data;
	struct input_dev *input;
	u32 *keycode;
	int count, i, ret;

	hall_info("xiaomi_hall_probe start.");

	drv_data = devm_kmalloc(dev, sizeof(*drv_data), GFP_KERNEL | __GFP_ZERO);
	if (!drv_data) {
		hall_err("No memory for xiaomi_hall_drv_data.");
		return -ENOMEM;
	}

	count = device_get_child_node_count(dev);
	hall_info("devicetree has %d child %s.", count, count < 2 ? "node" : "nodes");

	drv_data->hall_data = devm_kmalloc(dev, count * sizeof(*hall_data),
					   GFP_KERNEL | __GFP_ZERO);
	if (!drv_data->hall_data) {
		hall_err("No memory for xiaomi_hall_data.");
		devm_kfree(dev, drv_data);
		return -ENOMEM;
	}
	drv_data->hall_count = count;

	ret = xiaomi_hall_parse_dt(dev, drv_data);
	if (ret)
		return ret;

	input = devm_input_allocate_device(dev);
	if (!input) {
		hall_err("alloc input memory fail.");
		return -ENOMEM;
	}

	input->dev.parent = dev;
	input->name = XIAOMI_HALL_COMPATIBLE;
	input->phys = XIAOMI_HALL_COMPATIBLE "/input0";
	input->open = xiaomi_hall_open;
	input->close = xiaomi_hall_close;
	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0efa;
	input->id.product = 0x0001;
	input->id.version = 0x1000;

	keycode = devm_kmalloc(dev, count * sizeof(*keycode), GFP_KERNEL | __GFP_ZERO);
	if (!keycode) {
		hall_err("alloc input memory fail.");
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		hall_data = &drv_data->hall_data[i];

		input_set_capability(input, hall_data->type, hall_data->code);
		keycode[i] = hall_data->code;
	}

	input->keycode = keycode;
	input->keycodesize = sizeof(*keycode);
	input->keycodemax = count;

	drv_data->input = input;
	dev_set_drvdata(&input->dev, drv_data);

	for (i = 0; i < count; i++)
		drv_data->hall_data[i].input = input;

	ret = input_register_device(input);
	if (ret) {
		hall_err("register input device fail, err=%d.", ret);
		return ret;
	}
	hall_info("register input device done.");

	ret = xiaomi_hall_register_irqs(dev, drv_data);
	if (ret)
		return ret;

	return xiaomi_hall_chardev_init(drv_data);
}

static int xiaomi_hall_remove(struct platform_device *pdev)
{
	hall_info("xiaomi_hall_remove start.");

	device_destroy(xiaomi_hall_dev.class,
		       MKDEV(MAJOR(xiaomi_hall_dev.devt), 0));
	class_destroy(xiaomi_hall_dev.class);
	cdev_del(&xiaomi_hall_dev.cdev);
	unregister_chrdev_region(xiaomi_hall_dev.devt, 1);

	return 0;
}

static const struct of_device_id xiaomi_hall_of_match[] = {
	{ .compatible = XIAOMI_HALL_COMPATIBLE },
	{ }
};

static struct platform_driver xiaomi_hall_driver = {
	.probe = xiaomi_hall_probe,
	.remove = xiaomi_hall_remove,
	.driver = {
		.name = XIAOMI_HALL_COMPATIBLE,
		.of_match_table = xiaomi_hall_of_match,
	},
};

module_platform_driver(xiaomi_hall_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wushukong <wushukong@xiaomi.com>");
