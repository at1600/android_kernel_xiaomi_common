#include <linux/module.h>
#include <linux/device.h>
#include <linux/thermal.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/power_supply.h>
#include <drm/mi_disp_notifier.h>

struct thermal_msg_priv {
	int usb_online;
	int screen_state;
	struct notifier_block screen_nb;
	struct notifier_block usb_nb;
};

static struct thermal_zone_device *tzd;

static int msg_get_temp(struct thermal_zone_device *tz, int *temp)
{
	*temp = 25000;
	return 0;
}

static const struct thermal_zone_device_ops msg_tz_ops = {
	.get_temp = msg_get_temp,
};

static ssize_t screen_state_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct thermal_msg_priv *priv = thermal_zone_device_priv(tzd);
	return sysfs_emit(buf, "%d\n", priv->screen_state);
}
static DEVICE_ATTR_RO(screen_state);

static ssize_t usb_online_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct thermal_msg_priv *priv = thermal_zone_device_priv(tzd);
	return sysfs_emit(buf, "%d\n", priv->usb_online);
}
static DEVICE_ATTR_RO(usb_online);

static struct attribute *msg_attrs[] = {
	&dev_attr_screen_state.attr,
	&dev_attr_usb_online.attr,
	NULL,
};

static const struct attribute_group msg_attr_group = {
	.attrs = msg_attrs,
};

static const char *screen_state_name(int state)
{
	switch (state) {
	case MI_DISP_DPMS_ON:		return "On";
	case MI_DISP_DPMS_LP1:		return "Doze";
	case MI_DISP_DPMS_LP2:		return "DozeSuspend";
	case MI_DISP_DPMS_POWERDOWN:	return "Off";
	default:			return "Unknown";
	}
}

static int screen_state_notify(struct notifier_block *nb,
			       unsigned long val, void *data)
{
	struct thermal_msg_priv *priv = container_of(nb, struct thermal_msg_priv, screen_nb);
	struct mi_disp_notifier *ev = data;
	int state;

	if (val != MI_DISP_DPMS_EVENT || !ev || !ev->data)
		return NOTIFY_OK;

	state = *(int *)ev->data;
	priv->screen_state = (state == MI_DISP_DPMS_ON) ? 1 : 0;

	pr_debug("thermal_msg: screen %s, state=%d\n",
		 screen_state_name(state), priv->screen_state);

	sysfs_notify(&tzd->dev.kobj, NULL, "screen_state");
	return NOTIFY_OK;
}

static int usb_online_notify(struct notifier_block *nb,
			     unsigned long val, void *data)
{
	struct thermal_msg_priv *priv = container_of(nb, struct thermal_msg_priv, usb_nb);
	struct power_supply *psy = data;
	union power_supply_propval prop;

	if (strcmp(psy->desc->name, "usb"))
		return NOTIFY_OK;

	if (power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &prop) == 0) {
		priv->usb_online = prop.intval;
		sysfs_notify(&tzd->dev.kobj, NULL, "usb_online");
	}

	return NOTIFY_OK;
}

static int __init thermal_msg_init(void)
{
	struct thermal_msg_priv *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	tzd = thermal_zone_device_alloc("thermal_message", 0, 0,
					priv, &msg_tz_ops, NULL, 0, 0);
	if (IS_ERR(tzd)) {
		ret = PTR_ERR(tzd);
		goto err_free_priv;
	}

	ret = thermal_zone_device_add(tzd);
	if (ret)
		goto err_put_tzd;

	ret = sysfs_create_group(&tzd->dev.kobj, &msg_attr_group);
	if (ret)
		goto err_del_tzd;

	priv->screen_nb.notifier_call = screen_state_notify;
	mi_disp_register_client(&priv->screen_nb);

	priv->usb_nb.notifier_call = usb_online_notify;
	power_supply_reg_notifier(&priv->usb_nb);

	return 0;

err_del_tzd:
	thermal_zone_device_del(tzd);
err_put_tzd:
	thermal_zone_device_put(tzd);
err_free_priv:
	kfree(priv);
	return ret;
}

static void __exit thermal_msg_exit(void)
{
	struct thermal_msg_priv *priv = thermal_zone_device_priv(tzd);

	power_supply_unreg_notifier(&priv->usb_nb);
	mi_disp_unregister_client(&priv->screen_nb);

	sysfs_remove_group(&tzd->dev.kobj, &msg_attr_group);
	thermal_zone_device_del(tzd);
	thermal_zone_device_put(tzd);
	kfree(priv);
}

module_init(thermal_msg_init);
module_exit(thermal_msg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Thermal message interface for 6.12 kernel");
MODULE_ALIAS("thermal:thermal_message");
