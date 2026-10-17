/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Xiaomi keyboard companion driver - shared definitions.
 *
 * The vendor ships this as xiaomi_keyboard_driver.ko next to the Nanosic
 * keyboard driver.  It is not a keyboard driver itself: it owns the
 * /sys/class/keyboard/{keypad,touchpad} nodes, the /proc event rings and the
 * touchpad edge filter that the Nanosic driver calls into.
 */

#ifndef _XIAOMI_KEYBOARD_H
#define _XIAOMI_KEYBOARD_H

#include <linux/device.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/time64.h>
#include <linux/types.h>

#define XIAOMI_TOUCHPAD_SLOT_MAX	3
#define XIAOMI_KEYBOARD_EVENT_MAX	1024
#define XIAOMI_TOUCHPAD_EDGE_PARAM_MAX	14

/*
 * Nanosic touch report layout.  Byte 0 carries the packet type (0x19), byte 1
 * the button bits, bytes 2..19 three 6-byte points and byte 20 the number of
 * valid contacts.
 */
#define XIAOMI_TOUCHPAD_REPORT_TYPE	0x19
#define XIAOMI_TOUCHPAD_REPORT_SIZE	21

enum xiaomi_touchpad_point_state {
	XIAOMI_POINT_INIT = 0,
	XIAOMI_POINT_DOWN,
	XIAOMI_POINT_MOVE,
	XIAOMI_POINT_UP,
};

enum xiaomi_touchpad_slot_state {
	XIAOMI_SLOT_INIT = 0,
	XIAOMI_SLOT_RELEASE,
	XIAOMI_SLOT_NORMAL,
	XIAOMI_SLOT_EDGE_ACTIVE,
	XIAOMI_SLOT_EDGE_SHIELD,
};

enum xiaomi_touchpad_gesture {
	XIAOMI_GESTURE_NONE = 0,
	XIAOMI_GESTURE_ONE_SWIPE,
	XIAOMI_GESTURE_TWO_SWIPE,
	XIAOMI_GESTURE_TWO_PINCH,
	XIAOMI_GESTURE_THREE_SWIPE,
};

/* One 6-byte HID contact, as emitted by the Nanosic firmware. */
struct xiaomi_touchpad_point {
	u8 tip_switch:1;
	u8 reserved:1;
	u8 touch_valid:1;
	u8 contact_identifier;
	__le16 pos_x;
	__le16 pos_y;
};

struct xiaomi_touchpad_report {
	u8 type;
	u8 button_1:1;
	u8 button_2:1;
	u8 reserved:6;
	struct xiaomi_touchpad_point point[XIAOMI_TOUCHPAD_SLOT_MAX];
	u8 contact_count;
} __packed;

/* 14 tunables read from the touchpad,edge-* devicetree properties. */
struct xiaomi_touchpad_edge_filter_param {
	u32 edge_area_left_min;
	u32 edge_area_left_max;
	u32 edge_area_right_min;
	u32 edge_area_right_max;
	u32 edge_area_top_min;
	u32 edge_area_top_max;
	u32 edge_area_bottom_min;
	u32 edge_area_bottom_max;
	u32 shield_frame_count_limit;
	u32 one_swipe_dis_limit;
	u32 two_swipe_dis_limit;
	u32 two_pinch_dis_limit;
	u32 three_swipe_dis_limit;
	u32 two_fingers_gesture_dis;
};

struct xiaomi_touchpad_slot {
	u32 point_should_delete_count;
	u64 edge_point_move_distance;
	enum xiaomi_touchpad_point_state point_state;
	enum xiaomi_touchpad_slot_state slot_state;
	u8 slot_info_need_report;
	struct xiaomi_touchpad_point slot_info;
	struct xiaomi_touchpad_point first_edge_point;
};

struct xiaomi_keypad_event {
	const char *dev_name;
	u32 code;
	u32 value;
	struct timespec64 event_time;
};

struct xiaomi_touchpad_event {
	const char *dev_name;
	u32 slot_num;
	u32 tracking_id_num;
	bool contact_state;
	u32 tool_type;
	bool button_state;
	struct timespec64 event_time;
};

struct xiaomi_keypad_ring {
	u32 head;
	struct xiaomi_keypad_event buf[XIAOMI_KEYBOARD_EVENT_MAX];
};

struct xiaomi_touchpad_ring {
	u32 head;
	struct xiaomi_touchpad_event buf[XIAOMI_KEYBOARD_EVENT_MAX];
};

struct xiaomi_keypad_pdata {
	struct class *class;
	dev_t devt;
	struct device *dev;
	struct attribute_group attr_group;
	spinlock_t events_lock;
	struct proc_dir_entry *events_proc;
	struct xiaomi_keypad_ring events;
	/* Exposed through the keyboard_status_param attribute, see notify(). */
	u32 status;
	u16 status_code;
};

struct xiaomi_touchpad_pdata {
	struct class *class;
	dev_t devt;
	struct device *dev;
	struct attribute_group attr_group;
	u8 edge_filter_enable;
	u8 cdgain_robot_test_mode;
	struct xiaomi_touchpad_slot slots[XIAOMI_TOUCHPAD_SLOT_MAX];
	struct mutex edge_filter_mutex;
	struct xiaomi_touchpad_edge_filter_param edge_filter_param;
	spinlock_t events_lock;
	struct proc_dir_entry *events_proc;
	struct xiaomi_touchpad_ring events;
};

struct xiaomi_keyboard_pdata {
	struct device *dev;
	struct class *class;
	struct xiaomi_touchpad_pdata touchpad;
	struct xiaomi_keypad_pdata keypad;
};

/* Called by the Nanosic driver on probe and on runtime id changes. */
int xiaomi_touchpad_edge_filter_param_init(struct device_node *node);
int xiaomi_touchpad_edge_filter_param_set(struct device_node *node, u16 pid);
int xiaomi_touchpad_edge_filter_param_dynamic_change(struct device_node *node,
						     u16 pid);

/* Called for every touch report, the report is rewritten in place. */
int xiaomi_touchpad_edge_filter(struct xiaomi_touchpad_report *report);

/* Pushes the keyboard status to userspace through sysfs_notify(). */
void xiaomi_keypad_notify(u32 status, u16 status_code);

#endif /* _XIAOMI_KEYBOARD_H */
