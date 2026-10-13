#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/thermal.h>
#include <drm/mi_disp_notifier.h>
#include <net/netlink.h>
#include <net/genetlink.h>
#include <linux/power_supply.h>

static struct device *mi_thermal_dev;
static int usb_online; /* 替代原结构体里的 usb_online 成员 */

struct screen_monitor {
	struct notifier_block thermal_notifier;
	int screen_state;
};
struct screen_monitor sm;

static ssize_t thermal_screen_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sm.screen_state);
}
static DEVICE_ATTR(screen_state, 0664, thermal_screen_state_show, NULL);

static ssize_t thermal_usb_online_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", usb_online);
}
static DEVICE_ATTR(usb_online, 0664, thermal_usb_online_show, NULL);

static struct attribute *mi_thermal_dev_attr_group[] = {
	&dev_attr_screen_state.attr,
	&dev_attr_usb_online.attr,
	NULL,
};

static const struct attribute_group mi_thermal_attr_group = {
	.attrs = mi_thermal_dev_attr_group,
};

static const char *get_screen_state_name(int mode)
{
	switch (mode) {
	case MI_DISP_DPMS_ON:
		return "On";
	case MI_DISP_DPMS_LP1:
		return "Doze";
	case MI_DISP_DPMS_LP2:
		return "DozeSuspend";
	case MI_DISP_DPMS_POWERDOWN:
		return "Off";
	default:
		return "Unknown";
	}
}

static int screen_state_for_thermal_callback(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct mi_disp_notifier *evdata = data;
	unsigned int blank;

	if (val != MI_DISP_DPMS_EVENT || !evdata || !evdata->data)
		return 0;

	blank = *(int *)(evdata->data);
	switch (blank) {
	case MI_DISP_DPMS_ON:
		sm.screen_state = 1;
		break;
	case MI_DISP_DPMS_LP1:
	case MI_DISP_DPMS_LP2:
	case MI_DISP_DPMS_POWERDOWN:
		sm.screen_state = 0;
		break;
	default:
		break;
	}

	pr_warn("%s: %s, sm.screen_state = %d\n", __func__, get_screen_state_name(blank),
			sm.screen_state);

	sysfs_notify(&mi_thermal_dev->kobj, NULL, "screen_state");
	return NOTIFY_OK;
}

static int usb_online_callback(struct notifier_block *nb,
		unsigned long val, void *data)
{
	static struct power_supply *usb_psy;
	struct power_supply *psy = data;
	union power_supply_propval ret = {0,};
	int err = 0;

	if (strcmp(psy->desc->name, "usb"))
		return NOTIFY_OK;

	if (!usb_psy)
		usb_psy = power_supply_get_by_name("usb");

	if (usb_psy) {
		err = power_supply_get_property(usb_psy,
				POWER_SUPPLY_PROP_ONLINE, &ret);
		if (err) {
			pr_err("usb online read error:%d\n", err);
			return err;
		}
		usb_online = ret.intval;
		sysfs_notify(&mi_thermal_dev->kobj, NULL, "usb_online");
	}
	return NOTIFY_OK;
}

static int create_thermal_message_node(void)
{
	int ret = 0;
	struct class *thermal_class;

	thermal_class = class_find_by_name("thermal");
	if (!thermal_class) {
		pr_err("%s ERROR: thermal class not found\n", __func__);
		return -ENODEV;
	}

	mi_thermal_dev = device_create(thermal_class, NULL, MKDEV(0, 0),
					NULL, "thermal_message");
	if (IS_ERR(mi_thermal_dev)) {
		pr_err("%s ERROR: Cannot create thermal message device\n", __func__);
		ret = PTR_ERR(mi_thermal_dev);
		return ret;
	}

	ret = sysfs_create_group(&mi_thermal_dev->kobj, &mi_thermal_attr_group);
	if (ret) {
		pr_err("%s ERROR: Cannot create sysfs structure!:%d\n", __func__, ret);
		device_destroy(thermal_class, MKDEV(0, 0));
		ret = -ENODEV;
		return ret;
	}

	return ret;
}

static void destroy_thermal_message_node(void)
{
	struct class *thermal_class;

	thermal_class = class_find_by_name("thermal");
	if (thermal_class && mi_thermal_dev) {
		sysfs_remove_group(&mi_thermal_dev->kobj, &mi_thermal_attr_group);
		device_destroy(thermal_class, MKDEV(0, 0));
		mi_thermal_dev = NULL;
	}
}

static int __init mi_thermal_interface_init(void)
{
	int ret;

	ret = create_thermal_message_node();
	if (ret)
		return ret;


	return 0;
}

static void __exit mi_thermal_interface_exit(void)
{
	destroy_thermal_message_node();
}

module_init(mi_thermal_interface_init);
module_exit(mi_thermal_interface_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fankl");
MODULE_DESCRIPTION("Xiaomi thermal control interface");
MODULE_LICENSE("GPL v2");
