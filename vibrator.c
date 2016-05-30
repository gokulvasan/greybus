/*
 * Greybus Vibrator protocol driver.
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/idr.h>
#include "greybus.h"

struct gb_vibrator_device {
	struct gb_connection	*connection;
	struct device		*dev;
	int			minor;		/* vibrator minor number */
};

/* Greybus Vibrator operation types */
#define	GB_VIBRATOR_TYPE_ON			0x02
#define	GB_VIBRATOR_TYPE_OFF			0x03

struct gb_vibrator_on_request {
	__le16	timeout_ms;
};

static int turn_on(struct gb_vibrator_device *vib, u16 timeout_ms)
{
	struct gb_vibrator_on_request request;

	request.timeout_ms = cpu_to_le16(timeout_ms);
	return gb_operation_sync(vib->connection, GB_VIBRATOR_TYPE_ON,
				 &request, sizeof(request), NULL, 0);
}

static int turn_off(struct gb_vibrator_device *vib)
{
	return gb_operation_sync(vib->connection, GB_VIBRATOR_TYPE_OFF,
				 NULL, 0, NULL, 0);
}

static ssize_t timeout_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct gb_vibrator_device *vib = dev_get_drvdata(dev);
	unsigned long val;
	int retval;

	retval = kstrtoul(buf, 10, &val);
	if (retval < 0) {
		dev_err(dev, "could not parse timeout value %d\n", retval);
		return retval;
	}

	if (val)
		retval = turn_on(vib, (u16)val);
	else
		retval = turn_off(vib);
	if (retval)
		return retval;

	return count;
}
static DEVICE_ATTR_WO(timeout);

static struct attribute *vibrator_attrs[] = {
	&dev_attr_timeout.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vibrator);

static struct class vibrator_class = {
	.name		= "vibrator",
	.owner		= THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
	.dev_groups	= vibrator_groups,
#endif
};

static DEFINE_IDA(minors);

static int gb_vibrator_probe(struct gb_bundle *bundle,
					const struct greybus_bundle_id *id)
{
	struct greybus_descriptor_cport *cport_desc;
	struct gb_connection *connection;
	struct gb_vibrator_device *vib;
	struct device *dev;
	int retval;

	if (bundle->num_cports != 1)
		return -ENODEV;

	cport_desc = &bundle->cport_desc[0];
	if (cport_desc->protocol_id != GREYBUS_PROTOCOL_VIBRATOR)
		return -ENODEV;

	vib = kzalloc(sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;

	connection = gb_connection_create(bundle, le16_to_cpu(cport_desc->id),
						NULL);
	if (IS_ERR(connection)) {
		retval = PTR_ERR(connection);
		goto err_free_vib;
	}
	gb_connection_set_data(connection, vib);

	vib->connection = connection;

	greybus_set_drvdata(bundle, vib);

	retval = gb_connection_enable(connection);
	if (retval)
		goto err_connection_destroy;

	/*
	 * For now we create a device in sysfs for the vibrator, but odds are
	 * there is a "real" device somewhere in the kernel for this, but I
	 * can't find it at the moment...
	 */
	vib->minor = ida_simple_get(&minors, 0, 0, GFP_KERNEL);
	if (vib->minor < 0) {
		retval = vib->minor;
		goto err_connection_disable;
	}
	dev = device_create(&vibrator_class, &bundle->dev,
			    MKDEV(0, 0), vib, "vibrator%d", vib->minor);
	if (IS_ERR(dev)) {
		retval = -EINVAL;
		goto err_ida_remove;
	}
	vib->dev = dev;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
	/*
	 * Newer kernels handle this in a race-free manner, by the dev_groups
	 * field in the struct class up above.  But for older kernels, we need
	 * to "open code this :(
	 */
	retval = sysfs_create_group(&dev->kobj, vibrator_groups[0]);
	if (retval) {
		device_unregister(dev);
		goto err_ida_remove;
	}
#endif

	return 0;

err_ida_remove:
	ida_simple_remove(&minors, vib->minor);
err_connection_disable:
	gb_connection_disable(connection);
err_connection_destroy:
	gb_connection_destroy(connection);
err_free_vib:
	kfree(vib);

	return retval;
}

static void gb_vibrator_disconnect(struct gb_bundle *bundle)
{
	struct gb_vibrator_device *vib = greybus_get_drvdata(bundle);

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
	sysfs_remove_group(&vib->dev->kobj, vibrator_groups[0]);
#endif
	device_unregister(vib->dev);
	ida_simple_remove(&minors, vib->minor);
	gb_connection_disable(vib->connection);
	gb_connection_destroy(vib->connection);
	kfree(vib);
}

static const struct greybus_bundle_id gb_vibrator_id_table[] = {
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_VIBRATOR) },
	{ }
};
MODULE_DEVICE_TABLE(greybus, gb_vibrator_id_table);

static struct greybus_driver gb_vibrator_driver = {
	.name		= "vibrator",
	.probe		= gb_vibrator_probe,
	.disconnect	= gb_vibrator_disconnect,
	.id_table	= gb_vibrator_id_table,
};

static __init int gb_vibrator_init(void)
{
	int retval;

	retval = class_register(&vibrator_class);
	if (retval)
		return retval;

	retval = greybus_register(&gb_vibrator_driver);
	if (retval)
		goto err_class_unregister;

	return 0;

err_class_unregister:
	class_unregister(&vibrator_class);

	return retval;
}
module_init(gb_vibrator_init);

static __exit void gb_vibrator_exit(void)
{
	greybus_deregister(&gb_vibrator_driver);
	class_unregister(&vibrator_class);
	ida_destroy(&minors);
}
module_exit(gb_vibrator_exit);

MODULE_LICENSE("GPL v2");
