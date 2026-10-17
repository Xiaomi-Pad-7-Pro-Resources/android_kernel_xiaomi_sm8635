// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi keyboard companion driver.
 *
 * The Nanosic keyboard/touchpad driver talks to userspace through this module:
 * it loads the touchpad edge filter tunables from devicetree, runs every touch
 * report through the edge filter and reports keyboard status changes through
 * sysfs.  The module itself never touches the hardware.
 *
 * Userspace facing interfaces:
 *   /sys/class/keyboard/keypad/keyboard_status_param
 *   /sys/class/keyboard/touchpad/edge_filter_param
 *   /sys/class/keyboard/touchpad/edge_filter_enable
 *   /sys/class/keyboard/touchpad/cdgain_robot_test_mode
 *   /proc/last_keypad_events
 *   /proc/last_touchpad_events
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/rtc.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/timekeeping.h>

#include <linux/xiaomi_keyboard.h>

#define XIAOMI_KEYBOARD_TAG	"xiaomi-keyboard"

#define kbd_print(level, fmt, ...)					\
do {									\
	struct timespec64 __ts;						\
	struct rtc_time __tm;						\
									\
	ktime_get_real_ts64(&__ts);					\
	rtc_time64_to_tm(__ts.tv_sec - sys_tz.tz_minuteswest * 60, &__tm); \
	pr_##level("[%02d:%02d:%02d.%03zu] [%s] %s %d: " fmt,		\
		   __tm.tm_hour, __tm.tm_min, __tm.tm_sec,		\
		   (size_t)(__ts.tv_nsec / 1000000),			\
		   XIAOMI_KEYBOARD_TAG, __func__, __LINE__,		\
		   ##__VA_ARGS__);					\
} while (0)

#define XIAOMI_TOUCHPAD_EDGE_CONFIG	"touchpad-configs"

static struct xiaomi_keyboard_pdata *keyboard_pdata;

static const char *const xiaomi_touchpad_edge_param_name[] = {
	"touchpad,edge-area-left-min",
	"touchpad,edge-area-left-max",
	"touchpad,edge-area-right-min",
	"touchpad,edge-area-right-max",
	"touchpad,edge-area-top-min",
	"touchpad,edge-area-top-max",
	"touchpad,edge-area-bottom-min",
	"touchpad,edge-area-bottom-max",
	"touchpad,shield-frame-count-limit",
	"touchpad,one-swipe-dis-limit",
	"touchpad,two-swipe-dis-limit",
	"touchpad,two-pinch-dis-limit",
	"touchpad,three-swipe-dis-limit",
	"touchpad,two-fingers-gesture-dis",
};

static struct xiaomi_touchpad_pdata *xiaomi_keyboard_get_touchpad(void)
{
	if (!keyboard_pdata)
		return NULL;

	return &keyboard_pdata->touchpad;
}

static struct xiaomi_keypad_pdata *xiaomi_keyboard_get_keypad(void)
{
	if (!keyboard_pdata)
		return NULL;

	return &keyboard_pdata->keypad;
}

/* -------------------------------------------------------------------------
 * Keypad
 */

static ssize_t keyboard_status_param_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct xiaomi_keypad_pdata *pdata = dev_get_drvdata(dev);
	u64 value;

	if (!pdata)
		return -ENODEV;

	value = (u64)pdata->status | ((u64)pdata->status_code << 32);
	memcpy(buf, &value, sizeof(value));

	return sizeof(value);
}

static DEVICE_ATTR_RO(keyboard_status_param);

static struct attribute *xiaomi_keypad_attrs[] = {
	&dev_attr_keyboard_status_param.attr,
	NULL,
};

static struct attribute_group xiaomi_keypad_attr_group = {
	.attrs = xiaomi_keypad_attrs,
};

static int xiaomi_keypad_sysfs_init(struct xiaomi_keypad_pdata *pdata)
{
	int ret;

	pdata->attr_group = xiaomi_keypad_attr_group;

	ret = sysfs_create_group(&pdata->dev->kobj, &pdata->attr_group);
	if (ret)
		kbd_print(err, "xiaomi_keypad_sysfs_init fail! ret = %d\n", ret);
	else
		kbd_print(info, "xiaomi_keypad_sysfs_init success!\n");

	return ret ? -ENODEV : 0;
}

static void xiaomi_keypad_sysfs_deinit(struct xiaomi_keypad_pdata *pdata)
{
	sysfs_remove_group(&pdata->dev->kobj, &pdata->attr_group);
	kbd_print(info, "xiaomi_keypad_sysfs_deinit success!\n");
}

void xiaomi_keypad_notify(u32 status, u16 status_code)
{
	struct xiaomi_keypad_pdata *pdata = xiaomi_keyboard_get_keypad();

	if (!pdata) {
		kbd_print(err, "[DIS-TF-KEYBOARD] get_keypad_pdata fail!\n");
		return;
	}

	pdata->status = status;
	pdata->status_code = status_code;
	sysfs_notify(&pdata->dev->kobj, NULL, "keyboard_status_param");
}
EXPORT_SYMBOL_GPL(xiaomi_keypad_notify);

static void *keypad_events_start(struct seq_file *m, loff_t *pos)
{
	struct xiaomi_keypad_pdata *pdata = xiaomi_keyboard_get_keypad();
	void *ret = NULL;

	if (!pdata)
		return NULL;

	spin_lock(&pdata->events_lock);
	if (*pos < XIAOMI_KEYBOARD_EVENT_MAX)
		ret = &pdata->events.buf[(pdata->events.head + *pos) &
					 (XIAOMI_KEYBOARD_EVENT_MAX - 1)];
	spin_unlock(&pdata->events_lock);

	return ret;
}

static void *keypad_events_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct xiaomi_keypad_pdata *pdata = xiaomi_keyboard_get_keypad();
	void *ret = NULL;

	if (!pdata)
		return NULL;

	spin_lock(&pdata->events_lock);
	if (++(*pos) < XIAOMI_KEYBOARD_EVENT_MAX)
		ret = &pdata->events.buf[(pdata->events.head + *pos) &
					 (XIAOMI_KEYBOARD_EVENT_MAX - 1)];
	spin_unlock(&pdata->events_lock);

	return ret;
}

static void keypad_events_stop(struct seq_file *m, void *v)
{
}

static int keypad_events_show(struct seq_file *m, void *v)
{
	struct xiaomi_keypad_event *event = v;
	struct rtc_time tm;

	if (!event)
		return -EINVAL;

	rtc_time64_to_tm(event->event_time.tv_sec, &tm);
	seq_printf(m,
		   "%d-%02d-%02d %02d:%02d:%02d.%09lu UTC Input device:[%s] Keycode:[%d] Keyvalue:[%d]\n",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec,
		   (unsigned long)event->event_time.tv_nsec,
		   event->dev_name ?: "unknown", event->code, event->value);

	return 0;
}

static const struct seq_operations last_keypad_events_seq_ops = {
	.start = keypad_events_start,
	.next = keypad_events_next,
	.stop = keypad_events_stop,
	.show = keypad_events_show,
};

static int xiaomi_keypad_init(struct class *class, dev_t devt,
			      struct xiaomi_keypad_pdata *pdata)
{
	int ret;

	pdata->class = class;
	pdata->devt = devt;
	pdata->dev = device_create(class, NULL, devt, pdata, "keypad");
	if (IS_ERR(pdata->dev)) {
		ret = PTR_ERR(pdata->dev);
		pdata->dev = NULL;
		return ret;
	}

	ret = xiaomi_keypad_sysfs_init(pdata);
	if (ret) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_keypad_init fail! ret = %d\n",
			  ret);
		device_destroy(pdata->class, pdata->devt);
		return ret;
	}

	kbd_print(info, "xiaomi_keypad_init success!\n");

	return 0;
}

static void xiaomi_keypad_deinit(struct xiaomi_keypad_pdata *pdata)
{
	if (!pdata->dev)
		return;

	xiaomi_keypad_sysfs_deinit(pdata);
	device_destroy(pdata->class, pdata->devt);
	pdata->dev = NULL;

	kbd_print(info, "xiaomi_keypad_deinit success!\n");
}

/* -------------------------------------------------------------------------
 * Touchpad sysfs
 */

static ssize_t edge_filter_param_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct xiaomi_touchpad_pdata *pdata = dev_get_drvdata(dev);
	u32 *param;
	ssize_t len = 0;
	int i;

	if (!pdata)
		return -ENODEV;

	param = (u32 *)&pdata->edge_filter_param;

	mutex_lock(&pdata->edge_filter_mutex);
	for (i = 0; i < XIAOMI_TOUCHPAD_EDGE_PARAM_MAX; i++)
		len += sysfs_emit_at(buf, len, "%s: %d\n",
				     xiaomi_touchpad_edge_param_name[i],
				     param[i]);
	mutex_unlock(&pdata->edge_filter_mutex);

	return len;
}

static ssize_t edge_filter_param_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct xiaomi_touchpad_pdata *pdata = dev_get_drvdata(dev);
	u32 *param;
	char *cursor = (char *)buf;
	int i;

	if (!pdata)
		return -ENODEV;

	param = (u32 *)&pdata->edge_filter_param;

	mutex_lock(&pdata->edge_filter_mutex);
	for (i = 0; i < XIAOMI_TOUCHPAD_EDGE_PARAM_MAX; i++) {
		char *next;

		param[i] = simple_strtoul(cursor, &next, 10);
		if (next == cursor)
			break;
		cursor = next;
		while (*cursor == ' ')
			cursor++;
	}
	mutex_unlock(&pdata->edge_filter_mutex);

	return count;
}

static DEVICE_ATTR_RW(edge_filter_param);

static ssize_t edge_filter_enable_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct xiaomi_touchpad_pdata *pdata = dev_get_drvdata(dev);
	const char *state;

	if (!pdata)
		return -ENODEV;

	mutex_lock(&pdata->edge_filter_mutex);
	state = pdata->edge_filter_enable ? "enable" : "disable";
	mutex_unlock(&pdata->edge_filter_mutex);

	return sysfs_emit(buf, "%s\n", state);
}

static ssize_t edge_filter_enable_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct xiaomi_touchpad_pdata *pdata = dev_get_drvdata(dev);

	if (!pdata)
		return -ENODEV;

	mutex_lock(&pdata->edge_filter_mutex);
	if (sysfs_streq(buf, "enable"))
		pdata->edge_filter_enable = 1;
	else if (sysfs_streq(buf, "disable"))
		pdata->edge_filter_enable = 0;
	mutex_unlock(&pdata->edge_filter_mutex);

	return count;
}

static DEVICE_ATTR_RW(edge_filter_enable);

static ssize_t cdgain_robot_test_mode_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct xiaomi_touchpad_pdata *pdata = dev_get_drvdata(dev);
	const char *state;

	if (!pdata)
		return -ENODEV;

	mutex_lock(&pdata->edge_filter_mutex);
	state = pdata->cdgain_robot_test_mode ? "enable" : "disable";
	mutex_unlock(&pdata->edge_filter_mutex);

	return sysfs_emit(buf, "%s\n", state);
}

static ssize_t cdgain_robot_test_mode_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct xiaomi_touchpad_pdata *pdata = dev_get_drvdata(dev);

	if (!pdata)
		return -ENODEV;

	mutex_lock(&pdata->edge_filter_mutex);
	if (sysfs_streq(buf, "enable"))
		pdata->cdgain_robot_test_mode = 1;
	else if (sysfs_streq(buf, "disable"))
		pdata->cdgain_robot_test_mode = 0;
	kbd_print(info, "cdgain_robot_test_mode = %d\n",
		  pdata->cdgain_robot_test_mode);
	mutex_unlock(&pdata->edge_filter_mutex);

	return count;
}

static DEVICE_ATTR_RW(cdgain_robot_test_mode);

static struct attribute *xiaomi_touchpad_attrs[] = {
	&dev_attr_edge_filter_param.attr,
	&dev_attr_edge_filter_enable.attr,
	&dev_attr_cdgain_robot_test_mode.attr,
	NULL,
};

static struct attribute_group xiaomi_touchpad_attr_group = {
	.attrs = xiaomi_touchpad_attrs,
};

static int xiaomi_touchpad_sysfs_init(struct xiaomi_touchpad_pdata *pdata)
{
	int ret;

	pdata->attr_group = xiaomi_touchpad_attr_group;

	ret = sysfs_create_group(&pdata->dev->kobj, &pdata->attr_group);
	if (ret)
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_sysfs_init fail! ret = %d\n",
			  ret);
	else
		kbd_print(info, "xiaomi_touchpad_sysfs_init success!\n");

	return ret ? -ENODEV : 0;
}

static void xiaomi_touchpad_sysfs_deinit(struct xiaomi_touchpad_pdata *pdata)
{
	sysfs_remove_group(&pdata->dev->kobj, &pdata->attr_group);
	kbd_print(info, "xiaomi_touchpad_sysfs_deinit success!\n");
}

/* -------------------------------------------------------------------------
 * Touchpad event ring
 */

static void *touchpad_events_start(struct seq_file *m, loff_t *pos)
{
	struct xiaomi_touchpad_pdata *pdata = xiaomi_keyboard_get_touchpad();
	void *ret = NULL;

	if (!pdata)
		return NULL;

	spin_lock(&pdata->events_lock);
	if (*pos < XIAOMI_KEYBOARD_EVENT_MAX)
		ret = &pdata->events.buf[(pdata->events.head + *pos) &
					 (XIAOMI_KEYBOARD_EVENT_MAX - 1)];
	spin_unlock(&pdata->events_lock);

	return ret;
}

static void *touchpad_events_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct xiaomi_touchpad_pdata *pdata = xiaomi_keyboard_get_touchpad();
	void *ret = NULL;

	if (!pdata)
		return NULL;

	spin_lock(&pdata->events_lock);
	if (++(*pos) < XIAOMI_KEYBOARD_EVENT_MAX)
		ret = &pdata->events.buf[(pdata->events.head + *pos) &
					 (XIAOMI_KEYBOARD_EVENT_MAX - 1)];
	spin_unlock(&pdata->events_lock);

	return ret;
}

static void touchpad_events_stop(struct seq_file *m, void *v)
{
}

static int touchpad_events_show(struct seq_file *m, void *v)
{
	struct xiaomi_touchpad_event *event = v;
	struct rtc_time tm;

	if (!event)
		return -EINVAL;

	rtc_time64_to_tm(event->event_time.tv_sec, &tm);
	seq_printf(m,
		   "%d-%02d-%02d %02d:%02d:%02d.%09lu UTC Input device:[%s] slot:[%d] contact_state:[%s] tool_type:[%d] button_state:[%s] id:[%d]\n",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec,
		   (unsigned long)event->event_time.tv_nsec,
		   event->dev_name ?: "unknown", event->slot_num,
		   event->contact_state ? "true" : "false", event->tool_type,
		   event->button_state ? "true" : "false",
		   event->tracking_id_num);

	return 0;
}

static const struct seq_operations last_touchpad_events_seq_ops = {
	.start = touchpad_events_start,
	.next = touchpad_events_next,
	.stop = touchpad_events_stop,
	.show = touchpad_events_show,
};

static int xiaomi_touchpad_init(struct class *class, dev_t devt,
				struct xiaomi_touchpad_pdata *pdata)
{
	int ret;

	pdata->class = class;
	pdata->devt = devt;
	pdata->dev = device_create(class, NULL, devt, pdata, "touchpad");
	if (IS_ERR(pdata->dev)) {
		ret = PTR_ERR(pdata->dev);
		pdata->dev = NULL;
		return ret;
	}

	ret = xiaomi_touchpad_sysfs_init(pdata);
	if (ret) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_init fail! ret = %d\n",
			  ret);
		device_destroy(pdata->class, pdata->devt);
		return ret;
	}

	mutex_init(&pdata->edge_filter_mutex);

	kbd_print(info, "xiaomi_touchpad_init success!\n");

	return 0;
}

static void xiaomi_touchpad_deinit(struct xiaomi_touchpad_pdata *pdata)
{
	if (!pdata->dev)
		return;

	xiaomi_touchpad_sysfs_deinit(pdata);
	device_destroy(pdata->class, pdata->devt);
	pdata->dev = NULL;

	kbd_print(info, "xiaomi_touchpad_deinit success!\n");
}

/* -------------------------------------------------------------------------
 * Touchpad edge filter
 */

/*
 * Reads the 14 touchpad,edge-* properties of @node into @param.  Called with
 * edge_filter_mutex held.
 */
static int xiaomi_touchpad_edge_filter_param_parse(struct device_node *node,
						   struct xiaomi_touchpad_edge_filter_param *param)
{
	u32 *values = (u32 *)param;
	int i, ret;

	for (i = 0; i < XIAOMI_TOUCHPAD_EDGE_PARAM_MAX; i++) {
		ret = of_property_read_u32(node, xiaomi_touchpad_edge_param_name[i],
					   &values[i]);
		if (ret) {
			kbd_print(err, "[DIS-TF-KEYBOARD] %s missing\n",
				  xiaomi_touchpad_edge_param_name[i]);
			return ret;
		}
	}

	return 0;
}

int xiaomi_touchpad_edge_filter_param_init(struct device_node *node)
{
	struct xiaomi_touchpad_pdata *pdata = xiaomi_keyboard_get_touchpad();
	int ret;

	if (!pdata) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_pdata err!\n");
		return -ENODEV;
	}

	if (!node) {
		kbd_print(err, "[DIS-TF-KEYBOARD] invalid devicetree node\n");
		return -EINVAL;
	}

	mutex_lock(&pdata->edge_filter_mutex);
	ret = xiaomi_touchpad_edge_filter_param_parse(node, &pdata->edge_filter_param);
	if (!ret)
		pdata->edge_filter_enable = 1;
	mutex_unlock(&pdata->edge_filter_mutex);

	if (!ret)
		kbd_print(info, "xiaomi_touchpad_edge_filter_param_init success!\n");

	return ret;
}
EXPORT_SYMBOL_GPL(xiaomi_touchpad_edge_filter_param_init);

/*
 * Walks the touchpad-configs child node and applies the tunables that match the
 * controller id @pid.  The Nanosic driver calls this once it knows which panel
 * it is talking to.
 */
int xiaomi_touchpad_edge_filter_param_set(struct device_node *node, u16 pid)
{
	struct xiaomi_touchpad_pdata *pdata = xiaomi_keyboard_get_touchpad();
	struct device_node *configs, *child;
	const char *compatible;
	int ret = 0;

	if (!pdata) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_pdata err!\n");
		return -ENODEV;
	}

	if (!node) {
		kbd_print(err, "[DIS-TF-KEYBOARD] invalid devicetree node\n");
		return -EINVAL;
	}

	switch (pid) {
	case 0x1294:
		compatible = "nanosic-pid-1294";
		break;
	case 0x1260:
		compatible = "nanosic-pid-1260";
		break;
	default:
		return 0;
	}

	mutex_lock(&pdata->edge_filter_mutex);

	configs = of_get_child_by_name(node, XIAOMI_TOUCHPAD_EDGE_CONFIG);
	if (!configs) {
		kbd_print(err, "[DIS-TF-KEYBOARD] touchpad-configs not found\n");
		goto out;
	}

	for_each_child_of_node(configs, child) {
		if (!of_device_is_compatible(child, compatible))
			continue;

		kbd_print(info, "load %s edge filter param\n", compatible);
		ret = xiaomi_touchpad_edge_filter_param_parse(child,
							      &pdata->edge_filter_param);
		break;
	}

	pdata->edge_filter_enable = 1;

out:
	mutex_unlock(&pdata->edge_filter_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(xiaomi_touchpad_edge_filter_param_set);

/*
 * Same as param_set() but for a panel whose tunables changed at runtime.  The
 * matching node is pid_1294_extend rather than the plain pid node.
 */
int xiaomi_touchpad_edge_filter_param_dynamic_change(struct device_node *node,
						     u16 pid)
{
	struct xiaomi_touchpad_pdata *pdata = xiaomi_keyboard_get_touchpad();
	struct device_node *configs, *child;
	int ret = 0;

	if (!pdata) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_pdata err!\n");
		return -ENODEV;
	}

	if (!node) {
		kbd_print(err, "[DIS-TF-KEYBOARD] invalid devicetree node\n");
		return -EINVAL;
	}

	mutex_lock(&pdata->edge_filter_mutex);

	configs = of_get_child_by_name(node, XIAOMI_TOUCHPAD_EDGE_CONFIG);
	if (!configs) {
		kbd_print(err, "[DIS-TF-KEYBOARD] touchpad-configs not found\n");
		goto out;
	}

	for_each_child_of_node(configs, child) {
		if (pid != 0x1294)
			continue;
		if (!of_device_is_compatible(child, "pid_1294_extend"))
			continue;

		ret = xiaomi_touchpad_edge_filter_param_parse(child,
							      &pdata->edge_filter_param);
		break;
	}

	pdata->edge_filter_enable = 1;

out:
	mutex_unlock(&pdata->edge_filter_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(xiaomi_touchpad_edge_filter_param_dynamic_change);

static bool xiaomi_touchpad_point_in_edge_area(
		const struct xiaomi_touchpad_edge_filter_param *param,
		u16 x, u16 y)
{
	if (x >= param->edge_area_left_min && x <= param->edge_area_left_max)
		return true;
	if (x >= param->edge_area_right_min && x <= param->edge_area_right_max)
		return true;
	if (y >= param->edge_area_top_min && y <= param->edge_area_top_max)
		return true;
	if (y >= param->edge_area_bottom_min && y <= param->edge_area_bottom_max)
		return true;

	return false;
}

/* Tracks the press/release transition of a single contact. */
static void xiaomi_touchpad_track_point(struct xiaomi_touchpad_slot *slot,
					const struct xiaomi_touchpad_point *point)
{
	slot->slot_info = *point;

	switch (slot->point_state) {
	case XIAOMI_POINT_INIT:
		slot->point_state = point->tip_switch ? XIAOMI_POINT_DOWN :
						       XIAOMI_POINT_INIT;
		break;
	case XIAOMI_POINT_DOWN:
	case XIAOMI_POINT_MOVE:
		slot->point_state = point->tip_switch ? XIAOMI_POINT_MOVE :
						       XIAOMI_POINT_UP;
		break;
	case XIAOMI_POINT_UP:
		slot->point_state = point->tip_switch ? XIAOMI_POINT_DOWN :
						       XIAOMI_POINT_UP;
		if (!point->tip_switch && !point->pos_y &&
		    !slot->first_edge_point.pos_y)
			slot->point_state = XIAOMI_POINT_INIT;
		break;
	}
}

static u64 xiaomi_touchpad_point_distance(const struct xiaomi_touchpad_point *a,
					  const struct xiaomi_touchpad_point *b)
{
	s64 dx = (s64)a->pos_x - (s64)b->pos_x;
	s64 dy = (s64)a->pos_y - (s64)b->pos_y;

	return (u64)(dx * dx + dy * dy);
}

static enum xiaomi_touchpad_gesture
xiaomi_touchpad_classify_gesture(struct xiaomi_touchpad_pdata *pdata,
				 const struct xiaomi_touchpad_slot **active,
				 int nr_active)
{
	const struct xiaomi_touchpad_edge_filter_param *param =
		&pdata->edge_filter_param;
	u64 distance;

	switch (nr_active) {
	case 1:
		return XIAOMI_GESTURE_ONE_SWIPE;
	case 2:
		distance = xiaomi_touchpad_point_distance(&active[0]->slot_info,
							  &active[1]->slot_info);
		if (distance >= param->two_fingers_gesture_dis)
			return XIAOMI_GESTURE_TWO_SWIPE;
		return XIAOMI_GESTURE_TWO_PINCH;
	case 3:
		return XIAOMI_GESTURE_THREE_SWIPE;
	default:
		return XIAOMI_GESTURE_NONE;
	}
}

static u32 xiaomi_touchpad_gesture_limit(struct xiaomi_touchpad_pdata *pdata,
					 enum xiaomi_touchpad_gesture gesture)
{
	const struct xiaomi_touchpad_edge_filter_param *param =
		&pdata->edge_filter_param;

	switch (gesture) {
	case XIAOMI_GESTURE_ONE_SWIPE:
		return param->one_swipe_dis_limit;
	case XIAOMI_GESTURE_TWO_SWIPE:
		return param->two_swipe_dis_limit;
	case XIAOMI_GESTURE_TWO_PINCH:
		return param->two_pinch_dis_limit;
	case XIAOMI_GESTURE_THREE_SWIPE:
		return param->three_swipe_dis_limit;
	default:
		return 0;
	}
}

static void xiaomi_touchpad_release_slot(struct xiaomi_touchpad_slot *slot,
					 enum xiaomi_touchpad_slot_state state)
{
	slot->point_should_delete_count = 0;
	slot->edge_point_move_distance = 0;
	memset(&slot->first_edge_point, 0, sizeof(slot->first_edge_point));
	slot->slot_state = state;
}

static void xiaomi_touchpad_run_slot(struct xiaomi_touchpad_pdata *pdata,
				     struct xiaomi_touchpad_slot *slot,
				     bool button_down)
{
	const struct xiaomi_touchpad_edge_filter_param *param =
		&pdata->edge_filter_param;
	bool in_edge;

	switch (slot->slot_state) {
	case XIAOMI_SLOT_INIT:
		if (slot->point_state != XIAOMI_POINT_DOWN)
			return;
		break;
	case XIAOMI_SLOT_RELEASE:
		if (slot->point_state == XIAOMI_POINT_INIT) {
			slot->slot_state = XIAOMI_SLOT_INIT;
			slot->slot_info_need_report = 1;
			return;
		}
		if (slot->point_state != XIAOMI_POINT_DOWN)
			return;
		break;
	case XIAOMI_SLOT_NORMAL:
		if (slot->point_state == XIAOMI_POINT_UP)
			xiaomi_touchpad_release_slot(slot, XIAOMI_SLOT_RELEASE);
		slot->slot_info_need_report = 1;
		return;
	case XIAOMI_SLOT_EDGE_ACTIVE:
		if (button_down) {
			slot->slot_state = XIAOMI_SLOT_NORMAL;
			slot->slot_info_need_report = 1;
			return;
		}
		if (slot->point_state == XIAOMI_POINT_UP) {
			slot->point_should_delete_count++;
			if (slot->point_should_delete_count >=
			    param->shield_frame_count_limit) {
				slot->slot_state = XIAOMI_SLOT_EDGE_SHIELD;
				return;
			}
		} else if (slot->point_state == XIAOMI_POINT_INIT) {
			slot->slot_state = XIAOMI_SLOT_INIT;
			slot->slot_info_need_report = 1;
			return;
		}

		in_edge = xiaomi_touchpad_point_in_edge_area(param,
							    slot->slot_info.pos_x,
							    slot->slot_info.pos_y);
		if (!in_edge && !pdata->cdgain_robot_test_mode) {
			slot->slot_state = XIAOMI_SLOT_NORMAL;
			slot->slot_info_need_report = 1;
		}
		return;
	case XIAOMI_SLOT_EDGE_SHIELD:
		if (button_down) {
			slot->slot_state = XIAOMI_SLOT_NORMAL;
			slot->slot_info_need_report = 1;
			return;
		}
		if (slot->point_state == XIAOMI_POINT_UP)
			xiaomi_touchpad_release_slot(slot, XIAOMI_SLOT_RELEASE);
		return;
	}

	/*
	 * INIT/RELEASE with a fresh press: decide whether this contact started
	 * in an edge area.  If it did the contact is shielded until it either
	 * leaves the area or the swipe is recognised.
	 */
	if (button_down) {
		slot->slot_state = XIAOMI_SLOT_NORMAL;
		slot->slot_info_need_report = 1;
		return;
	}

	if (xiaomi_touchpad_point_in_edge_area(param, slot->slot_info.pos_x,
					       slot->slot_info.pos_y)) {
		slot->slot_state = XIAOMI_SLOT_EDGE_ACTIVE;
		slot->first_edge_point = slot->slot_info;
		slot->point_should_delete_count = 0;
	} else {
		slot->slot_state = XIAOMI_SLOT_NORMAL;
		slot->slot_info_need_report = 1;
	}
}

/*
 * Runs every touch report through the edge filter.  Contacts that started in
 * an edge area are kept out of the report unless the gesture they belong to
 * travelled far enough to be a real swipe.  The report is rewritten in place.
 */
int xiaomi_touchpad_edge_filter(struct xiaomi_touchpad_report *report)
{
	const struct xiaomi_touchpad_slot *active[XIAOMI_TOUCHPAD_SLOT_MAX];
	struct xiaomi_touchpad_pdata *pdata = xiaomi_keyboard_get_touchpad();
	struct xiaomi_touchpad_report filtered;
	enum xiaomi_touchpad_gesture gesture;
	int nr_active = 0;
	bool button_down;
	u32 limit;
	int i;

	if (!pdata) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_pdata err!\n");
		return -ENODEV;
	}

	mutex_lock(&pdata->edge_filter_mutex);

	if (!pdata->edge_filter_enable) {
		mutex_unlock(&pdata->edge_filter_mutex);
		return -EINVAL;
	}

	button_down = report->button_1 || report->button_2;
	if (pdata->cdgain_robot_test_mode)
		button_down = false;

	for (i = 0; i < report->contact_count && i < XIAOMI_TOUCHPAD_SLOT_MAX; i++) {
		struct xiaomi_touchpad_point *point = &report->point[i];
		unsigned int id = point->contact_identifier;

		if (id >= XIAOMI_TOUCHPAD_SLOT_MAX)
			continue;

		xiaomi_touchpad_track_point(&pdata->slots[id], point);
	}

	for (i = 0; i < XIAOMI_TOUCHPAD_SLOT_MAX; i++)
		xiaomi_touchpad_run_slot(pdata, &pdata->slots[i], button_down);

	/* Gather the contacts that are still candidates for a gesture. */
	for (i = 0; i < XIAOMI_TOUCHPAD_SLOT_MAX; i++) {
		if (pdata->slots[i].slot_state != XIAOMI_SLOT_EDGE_ACTIVE)
			continue;
		if (nr_active >= XIAOMI_TOUCHPAD_SLOT_MAX)
			break;
		active[nr_active++] = &pdata->slots[i];
	}

	gesture = xiaomi_touchpad_classify_gesture(pdata, active, nr_active);
	limit = xiaomi_touchpad_gesture_limit(pdata, gesture);

	for (i = 0; i < nr_active; i++) {
		struct xiaomi_touchpad_slot *slot = (struct xiaomi_touchpad_slot *)active[i];

		slot->edge_point_move_distance =
			xiaomi_touchpad_point_distance(&slot->slot_info,
						       &slot->first_edge_point);
		if (slot->edge_point_move_distance < limit)
			continue;

		/* Far enough to be a swipe, let the contact through. */
		slot->slot_state = XIAOMI_SLOT_NORMAL;
		slot->slot_info_need_report = 1;
	}

	/* Rebuild the report from the contacts that survived the filter. */
	memset(&filtered, 0, sizeof(filtered));
	filtered.type = XIAOMI_TOUCHPAD_REPORT_TYPE;
	filtered.button_1 = report->button_1;
	filtered.button_2 = report->button_2;

	for (i = 0; i < XIAOMI_TOUCHPAD_SLOT_MAX; i++) {
		struct xiaomi_touchpad_slot *slot = &pdata->slots[i];

		if (!slot->slot_info_need_report)
			continue;
		slot->slot_info_need_report = 0;
		if (filtered.contact_count >= XIAOMI_TOUCHPAD_SLOT_MAX)
			continue;
		filtered.point[filtered.contact_count++] = slot->slot_info;
	}

	mutex_unlock(&pdata->edge_filter_mutex);

	*report = filtered;

	return 0;
}
EXPORT_SYMBOL_GPL(xiaomi_touchpad_edge_filter);

/* -------------------------------------------------------------------------
 * Input handler
 *
 * The vendor module registers a handler that watches the Nanosic input devices
 * and mirrors their events into the /proc rings.  Its id table is empty, so the
 * handler never binds to anything and the rings stay empty; the code is kept
 * for parity with the shipped module.
 */

static const char *const xiaomi_keyboard_device_name[] = {
	"Xiaomi Touch",
	"Xiaomi Consumer",
	"Xiaomi Keyboard",
};

static bool xiaomi_keyboard_device_matches(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xiaomi_keyboard_device_name); i++)
		if (sysfs_streq(name, xiaomi_keyboard_device_name[i]))
			return true;

	return false;
}

static void last_keypad_event_filter(const char *name, unsigned int type,
				     unsigned int code, int value)
{
	struct xiaomi_keypad_pdata *pdata = xiaomi_keyboard_get_keypad();
	struct xiaomi_keypad_event *event;
	u32 index;

	if (!pdata) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_keypad_pdata err!\n");
		return;
	}

	if (type != EV_KEY)
		return;

	spin_lock(&pdata->events_lock);
	index = pdata->events.head;
	event = &pdata->events.buf[index];
	event->dev_name = name;
	event->code = code;
	event->value = value;
	ktime_get_real_ts64(&event->event_time);
	pdata->events.head = (index + 1) & (XIAOMI_KEYBOARD_EVENT_MAX - 1);
	spin_unlock(&pdata->events_lock);
}

static void last_touchpad_event_filter(const char *name, unsigned int type,
				       unsigned int code, int value)
{
	struct xiaomi_touchpad_pdata *pdata = xiaomi_keyboard_get_touchpad();
	struct xiaomi_touchpad_event *event;
	static u32 slot_num;
	static u32 tracking_id;
	static bool contact_state[XIAOMI_TOUCHPAD_SLOT_MAX];
	static u32 tool_type[XIAOMI_TOUCHPAD_SLOT_MAX];
	static bool button_state;
	static bool contact_changed;
	static bool need_refresh;
	u32 index;

	if (!pdata) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_pdata err!\n");
		return;
	}

	spin_lock(&pdata->events_lock);

	if (type == EV_KEY && code == BTN_TOUCH) {
		button_state = !!value;
		need_refresh = true;
	} else if (type == EV_ABS && code == ABS_MT_SLOT) {
		slot_num = value;
	} else if (type == EV_ABS && code == ABS_MT_TRACKING_ID) {
		tracking_id = value;
		if (slot_num < XIAOMI_TOUCHPAD_SLOT_MAX) {
			contact_state[slot_num] = value != -1;
			contact_changed = true;
		}
	} else if (type == EV_ABS && code == ABS_MT_TOOL_TYPE) {
		if (slot_num < XIAOMI_TOUCHPAD_SLOT_MAX) {
			tool_type[slot_num] = value;
			contact_changed = true;
		}
	} else if (type == EV_SYN && code == SYN_REPORT && contact_changed) {
		need_refresh = true;
		contact_changed = false;
	}

	if (need_refresh && slot_num < XIAOMI_TOUCHPAD_SLOT_MAX) {
		index = pdata->events.head;
		event = &pdata->events.buf[index];
		event->dev_name = name;
		event->slot_num = slot_num;
		event->tracking_id_num = tracking_id;
		event->contact_state = contact_state[slot_num];
		event->tool_type = tool_type[slot_num];
		event->button_state = button_state;
		ktime_get_real_ts64(&event->event_time);
		pdata->events.head = (index + 1) & (XIAOMI_KEYBOARD_EVENT_MAX - 1);
		need_refresh = false;
	}

	spin_unlock(&pdata->events_lock);
}

static void xiaomi_keyboard_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	const char *name = handle->dev->name;

	if (!xiaomi_keyboard_device_matches(name))
		return;

	if (sysfs_streq(name, xiaomi_keyboard_device_name[0]))
		last_touchpad_event_filter(name, type, code, value);
	else
		last_keypad_event_filter(name, type, code, value);
}

static int xiaomi_keyboard_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	if (!xiaomi_keyboard_device_matches(dev->name))
		return -EINVAL;

	kbd_print(info, "connect %s\n", dev->name);

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "xiaomi_keyboard";

	ret = input_register_handle(handle);
	if (ret) {
		kfree(handle);
		return ret;
	}

	ret = input_open_device(handle);
	if (ret) {
		input_unregister_handle(handle);
		kfree(handle);
		return ret;
	}

	return 0;
}

static void xiaomi_keyboard_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/*
 * Empty id table, matching the shipped module: input_match_device() bails out on
 * the first entry because both flags and driver_info are zero, so the handler is
 * registered but never attaches to a device.
 */
static const struct input_device_id xiaomi_keyboard_ids[] = {
	{ },
};

static struct input_handler xiaomi_keyboard_input_handler = {
	.event = xiaomi_keyboard_input_event,
	.connect = xiaomi_keyboard_input_connect,
	.disconnect = xiaomi_keyboard_input_disconnect,
	.name = "xiaomi_keyboard",
	.id_table = xiaomi_keyboard_ids,
};

static int xiaomi_keyboard_evdev_init(void)
{
	struct xiaomi_keypad_pdata *keypad;
	struct xiaomi_touchpad_pdata *touchpad;

	if (!keyboard_pdata)
		return -EINVAL;

	keypad = &keyboard_pdata->keypad;
	touchpad = &keyboard_pdata->touchpad;

	keypad->events.head = 0;
	touchpad->events.head = 0;

	keypad->events_proc = proc_create_seq_private("last_keypad_events", 0444,
						      NULL, &last_keypad_events_seq_ops,
						      0, NULL);
	touchpad->events_proc = proc_create_seq_private("last_touchpad_events", 0444,
							NULL, &last_touchpad_events_seq_ops,
							0, NULL);

	return input_register_handler(&xiaomi_keyboard_input_handler);
}

static void xiaomi_keyboard_evdev_exit(void)
{
	struct xiaomi_keypad_pdata *keypad;
	struct xiaomi_touchpad_pdata *touchpad;

	if (!keyboard_pdata)
		return;

	keypad = &keyboard_pdata->keypad;
	touchpad = &keyboard_pdata->touchpad;

	remove_proc_entry("last_keypad_events", NULL);
	keypad->events_proc = NULL;
	remove_proc_entry("last_touchpad_events", NULL);
	touchpad->events_proc = NULL;

	input_unregister_handler(&xiaomi_keyboard_input_handler);
}

/* -------------------------------------------------------------------------
 * Module
 */

static const struct file_operations xiaomi_keyboard_fops = {
	.owner = THIS_MODULE,
};

static const char *const xiaomi_keyboard_dev_name = "xiaomi_keyboard";
static unsigned int xiaomi_keyboard_major;

static int __init xiaomi_keyboard_init(void)
{
	struct xiaomi_keyboard_pdata *pdata;
	int ret;

	kbd_print(info, "xiaomi_keyboard_probe!\n");

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->class = class_create(THIS_MODULE, "keyboard");
	if (IS_ERR(pdata->class)) {
		ret = PTR_ERR(pdata->class);
		pdata->class = NULL;
		goto err_free;
	}

	xiaomi_keyboard_major = __register_chrdev(0, 0, 0x100,
						  xiaomi_keyboard_dev_name,
						  &xiaomi_keyboard_fops);
	if ((int)xiaomi_keyboard_major < 0) {
		ret = (int)xiaomi_keyboard_major;
		goto err_class;
	}

	ret = xiaomi_keypad_init(pdata->class, MKDEV(xiaomi_keyboard_major, 0),
				 &pdata->keypad);
	if (ret) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_keypad_init fail! ret = %d\n",
			  ret);
		goto err_chrdev;
	}

	ret = xiaomi_touchpad_init(pdata->class, MKDEV(xiaomi_keyboard_major, 1),
				   &pdata->touchpad);
	if (ret) {
		kbd_print(err, "[DIS-TF-KEYBOARD] xiaomi_touchpad_init fail! ret = %d\n",
			  ret);
		goto err_keypad;
	}

	keyboard_pdata = pdata;

	xiaomi_keyboard_evdev_init();

	return 0;

err_keypad:
	xiaomi_keypad_deinit(&pdata->keypad);
err_chrdev:
	__unregister_chrdev(xiaomi_keyboard_major, 0, 0x100,
			    xiaomi_keyboard_dev_name);
err_class:
	class_destroy(pdata->class);
err_free:
	kfree(pdata);

	return ret;
}

static void __exit xiaomi_keyboard_exit(void)
{
	struct xiaomi_keyboard_pdata *pdata = keyboard_pdata;

	kbd_print(info, "xiaomi_keyboard_remove!\n");

	if (!pdata)
		return;

	xiaomi_keyboard_evdev_exit();
	xiaomi_touchpad_deinit(&pdata->touchpad);
	xiaomi_keypad_deinit(&pdata->keypad);
	__unregister_chrdev(xiaomi_keyboard_major, 0, 0x100,
			    xiaomi_keyboard_dev_name);
	class_destroy(pdata->class);

	keyboard_pdata = NULL;
	kfree(pdata);
}

module_init(xiaomi_keyboard_init);
module_exit(xiaomi_keyboard_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Xiaomi keyboard companion driver");
MODULE_AUTHOR("dodyirawan85 <dev.irawans@gmail.com>");
