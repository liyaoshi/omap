/*
 * Userspace interface to hardware spinlocks
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/hwspinlock.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <uapi/linux/hwspinlock_user.h>

#include <linux/miscdevice.h>

struct hwlock {
	struct hwspinlock *hwlock;
	struct file *owner;
};

struct hwspinlock_user {
	struct device *dev;
	struct mutex mutex;

	int num_locks;
	struct hwlock locks[0];
};

struct hwspinlock_user *user;

static long hwspinlock_user_ioctl(struct file *filp, unsigned int cmd,
				  unsigned long arg)
{
	struct hwspinlock *hwlock = NULL;
	union {
		struct hwspinlock_user_lock lock;
		struct hwspinlock_user_unlock unlock;
	} data;
	int i, id, ret = 0;

	if (copy_from_user(&data, (void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	mutex_unlock(&user->mutex);

	switch (cmd) {
	case HWSPINLOCK_USER_LOCK:
		for (i = 0; i < user->num_locks; i++) {
			id = hwspin_lock_get_id(user->locks[i].hwlock);
			if (id == data.lock.id) {
				hwlock = user->locks[i].hwlock;
				break;
			}
		}

		if (hwlock) {
			ret = hwspin_lock_timeout_can_sleep(hwlock,
							    data.lock.timeout);
			if (!ret)
				user->locks[i].owner = filp;
		} else {
			dev_err(user->dev, "hwspinlock %d is not reserved\n",
				data.lock.id);
			ret = -EINVAL;
		}
		break;

	case HWSPINLOCK_USER_UNLOCK:
		for (i = 0; i < user->num_locks; i++) {
			id = hwspin_lock_get_id(user->locks[i].hwlock);
			if (id == data.unlock.id) {
				hwlock = user->locks[i].hwlock;
				break;
			}
		}

		if (hwlock) {
			hwspin_unlock_can_sleep(hwlock);
			user->locks[i].owner = NULL;
		} else {
			dev_err(user->dev, "hwspinlock %d is not reserved\n",
				data.unlock.id);
			ret = -EINVAL;
		}
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&user->mutex);

	return ret;
}

static int hwspinlock_user_release(struct inode *inode, struct file *filp)
{
	int i;

	mutex_lock(&user->mutex);

	for (i = 0; i < user->num_locks; i++) {
		if (user->locks[i].owner == filp) {
			dev_warn(user->dev,
				 "hwspinlock %d is forcefully unlocked\n",
				 hwspin_lock_get_id(user->locks[i].hwlock));
			hwspin_unlock_can_sleep(user->locks[i].hwlock);
		}
	}

	mutex_unlock(&user->mutex);

	return 0;
}


static const struct file_operations hwspinlock_user_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= hwspinlock_user_ioctl,
	.release	= hwspinlock_user_release,
	.llseek		= noop_llseek,
};

static struct miscdevice hwspinlock_user_miscdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "hwspinlock",
	.fops		= &hwspinlock_user_fops,
};

static int hwspinlock_user_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct hwspinlock *hwlock;
	int num, id, i;
	int ret;

	if (!node)
		return -ENODEV;

	num = of_count_phandle_with_args(node, "hwlocks", "#hwlock-cells");

	user = devm_kzalloc(&pdev->dev, sizeof(struct hwspinlock_user) +
			      num * sizeof(struct hwlock), GFP_KERNEL);
	if (!user)
		return -ENOMEM;

	user->dev = &pdev->dev;
	user->num_locks = num;
	mutex_init(&user->mutex);

	ret = misc_register(&hwspinlock_user_miscdev);
	if (ret) {
		dev_err(user->dev, "failed to register miscdev %d\n", ret);
		return ret;
	}

	for (i = 0; i < user->num_locks; i++) {
		id = of_hwspin_lock_get_id(node, i);
		if (id < 0) {
			dev_err(user->dev, "failed to get lock id %d\n", id);
			ret = -ENODEV;
			goto err;
		}

		hwlock = hwspin_lock_request_specific(id);
		if (IS_ERR_OR_NULL(hwlock)) {
			dev_err(user->dev, "failed to request lock %d\n", id);
			ret = IS_ERR(hwlock) ? PTR_ERR(hwlock) : -EBUSY;
			goto err;
		}

		user->locks[i].hwlock = hwlock;
	}

	dev_info(user->dev, "requested %d hwspinlocks\n", i);

	platform_set_drvdata(pdev, user);

	return 0;

err:
	misc_deregister(&hwspinlock_user_miscdev);
	for (i--; i >= 0; i--)
		hwspin_lock_free(user->locks[i].hwlock);

	return ret;
}

static int hwspinlock_user_remove(struct platform_device *pdev)
{
	struct hwspinlock_user *user = platform_get_drvdata(pdev);
	int i;

	misc_deregister(&hwspinlock_user_miscdev);

	for (i = 0; i < user->num_locks; i++)
		hwspin_lock_free(user->locks[i].hwlock);

	return 0;
}

static const struct of_device_id hwspinlock_user_of_match[] = {
	{ .compatible = "hwspinlock-user", },
	{ /* end */ },
};
MODULE_DEVICE_TABLE(of, hwspinlock_user_of_match);

static struct platform_driver hwspinlock_user_driver = {
	.probe		= hwspinlock_user_probe,
	.remove		= hwspinlock_user_remove,
	.driver		= {
		.name	= "hwspinlock_user",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(hwspinlock_user_of_match),
	},
};

module_platform_driver(hwspinlock_user_driver);
