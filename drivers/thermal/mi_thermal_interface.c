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
#include "thermal_core.h"

/* ===================== 修复1：删除未定义的 mi_thermal_device，改用全局变量存业务数据 ===================== */
static struct thermal_zone_device *mi_tzd;
static int usb_online; /* 替代原来的 mi_thermal_dev.usb_online */

struct screen_monitor {
	struct notifier_block thermal_notifier;
	int screen_state;
};
struct screen_monitor sm;

/* ===================== 修复2：thermal zone 温度回调（必须实现） ===================== */
static int mi_thermal_get_temp(struct thermal_zone_device *tzd, int *temp)
{
	/* 替换为实际温度读取逻辑，单位：毫摄氏度 */
	*temp = 25000;
	return 0;
}

static const struct thermal_zone_device_ops mi_thermal_zone_ops = {
	.get_temp = mi_thermal_get_temp,
};

/* ===================== sysfs 属性节点 ===================== */
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

/* ===================== 修复3：属性组直接用标准结构体，不再嵌套一层 ===================== */
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

	/* ===================== 修复4：通知对象改为 thermal zone 自带的 device ===================== */
	sysfs_notify(&mi_tzd->dev.kobj, NULL, "screen_state");
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
		/* ===================== 修复5：通知对象改为 thermal zone 自带的 device ===================== */
		sysfs_notify(&mi_tzd->dev.kobj, NULL, "usb_online");
	}
	return NOTIFY_OK;
}

/* ===================== 修复6：创建 sysfs 节点，挂载到 thermal zone 设备上 ===================== */
static int create_thermal_message_node(void)
{
	int ret = 0;

	ret = sysfs_create_group(&mi_tzd->dev.kobj, &mi_thermal_attr_group);
	if (ret) {
		pr_err("%s ERROR: Cannot create sysfs structure!:%d\n", __func__, ret);
		ret = -ENODEV;
		return ret;
	}
	return ret;
}

static void destroy_thermal_message_node(void)
{
	sysfs_remove_group(&mi_tzd->dev.kobj, &mi_thermal_attr_group);
}

/* ===================== 修复7：6.12 标准两步注册法：alloc + add ===================== */
static int __init mi_thermal_interface_init(void)
{
	int ret;

	/* 第一步：分配 thermal zone 设备 */
	mi_tzd = thermal_zone_device_alloc(
		"mi_thermal",	/* 设备名，对应 /sys/class/thermal/mi_thermal */
		0,		/* trip 温差点数量 */
		0,		/* 中断掩码 */
		NULL,		/* 私有数据指针 */
		&mi_thermal_zone_ops,
		NULL,		/* 温控参数 */
		0,		/* 被动降温延迟(ms) */
		0		/* 轮询延迟(ms) */
	);
	if (IS_ERR(mi_tzd)) {
		pr_err("mi_thermal: alloc thermal zone failed, err=%ld\n",
		       PTR_ERR(mi_tzd));
		return PTR_ERR(mi_tzd);
	}

	/* 第二步：添加到内核 thermal 子系统 */
	ret = thermal_zone_device_add(mi_tzd);
	if (ret) {
		pr_err("mi_thermal: add thermal zone failed, err=%d\n", ret);
		goto err_put_tzd;
	}

	/* 创建自定义 sysfs 消息节点 */
	ret = create_thermal_message_node();
	if (ret)
		goto err_del_tzd;

	/* 你原来的其他初始化逻辑（通知注册等）保留在这里 */

	return 0;

err_del_tzd:
	thermal_zone_device_del(mi_tzd);
err_put_tzd:
	thermal_zone_device_put(mi_tzd);
	return ret;
}

static void __exit mi_thermal_interface_exit(void)
{
	/* 先销毁 sysfs 节点，再注销 thermal zone */
	destroy_thermal_message_node();

	if (mi_tzd) {
		thermal_zone_device_del(mi_tzd);
		thermal_zone_device_put(mi_tzd);
	}
}

module_init(mi_thermal_interface_init);
module_exit(mi_thermal_interface_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fankl");
MODULE_DESCRIPTION("Xiaomi thermal control interface");
MODULE_LICENSE("GPL v2");
