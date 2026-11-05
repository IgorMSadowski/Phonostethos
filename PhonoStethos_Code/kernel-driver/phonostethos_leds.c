// One character device accepts a complete four-LED mask from th_Main.
// GPIO descriptors come from phonostethos-status-leds-overlay.dts.
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define DEVICE_NAME "phonostethos_leds"
#define LED_ALL 0x0f
#define ERROR_PULSE 0xff

struct phonostethos_leds {
	struct gpio_desc *gpio[4]; // HEART, LUNG, GENERAL, RECORDING: bits 0..3.
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *device;
	struct mutex lock;
	struct delayed_work error_pulse_work;
	u8 cached_mask;
	bool available;
};

static struct phonostethos_leds leds = {
	.lock = __MUTEX_INITIALIZER(leds.lock),
};

static void set_led_mask(u8 mask)
{
	unsigned int index;

	// Descriptor polarity comes from GPIO_ACTIVE_HIGH in the overlay.
	for (index = 0; index < ARRAY_SIZE(leds.gpio); ++index)
		gpiod_set_value_cansleep(leds.gpio[index], !!(mask & BIT(index)));
}

static void restore_led_mask(struct work_struct *work)
{
	mutex_lock(&leds.lock);
	if (leds.available)
		set_led_mask(leds.cached_mask);
	mutex_unlock(&leds.lock);
}

static int phonostethos_leds_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(inode->i_cdev, struct phonostethos_leds, cdev);
	return 0;
}

static int phonostethos_leds_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t phonostethos_leds_write(struct file *file,
		const char __user *buffer, size_t length, loff_t *offset)
{
	struct phonostethos_leds *device = file->private_data;
	u8 command;

	if (length != sizeof(command))
		return -EINVAL;
	if (copy_from_user(&command, buffer, sizeof(command)))
		return -EFAULT;
	if (command != ERROR_PULSE && (command & ~LED_ALL))
		return -EINVAL;

	// Do not wait for work while holding the mutex that the work also needs.
	if (command != ERROR_PULSE)
		cancel_delayed_work_sync(&device->error_pulse_work);
	if (mutex_lock_interruptible(&device->lock))
		return -ERESTARTSYS;
	if (!device->available) {
		mutex_unlock(&device->lock);
		return -ENODEV;
	}
	if (command == ERROR_PULSE) {
		set_led_mask(LED_ALL);
		mod_delayed_work(system_wq, &device->error_pulse_work, msecs_to_jiffies(100));
	} else {
		device->cached_mask = command;
		set_led_mask(command);
	}
	mutex_unlock(&device->lock);
	return sizeof(command);
}

static const struct file_operations phonostethos_leds_fops = {
	.owner = THIS_MODULE,
	.open = phonostethos_leds_open,
	.release = phonostethos_leds_release,
	.write = phonostethos_leds_write,
	.llseek = noop_llseek,
};

static int phonostethos_leds_probe(struct platform_device *platform)
{
	static const char * const names[] = { "heart", "lung", "general", "recording" };
	unsigned int index;
	int ret;

	if (leds.device)
		return -EBUSY;
	// devm_ releases each descriptor if probe fails or the device is removed.
	for (index = 0; index < ARRAY_SIZE(names); ++index) {
		leds.gpio[index] = devm_gpiod_get(&platform->dev, names[index], GPIOD_OUT_LOW);
		if (IS_ERR(leds.gpio[index]))
			return dev_err_probe(&platform->dev, PTR_ERR(leds.gpio[index]),
					     "Cannot acquire %s GPIO\n", names[index]);
	}
	INIT_DELAYED_WORK(&leds.error_pulse_work, restore_led_mask);
	leds.cached_mask = 0;
	ret = alloc_chrdev_region(&leds.devt, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;
	cdev_init(&leds.cdev, &phonostethos_leds_fops);
	leds.cdev.owner = THIS_MODULE;
	ret = cdev_add(&leds.cdev, leds.devt, 1);
	if (ret)
		goto err_unregister;
	leds.class = class_create("phonostethos");
	if (IS_ERR(leds.class)) {
		ret = PTR_ERR(leds.class);
		goto err_cdev;
	}
	mutex_lock(&leds.lock);
	leds.available = true;
	mutex_unlock(&leds.lock);
	leds.device = device_create(leds.class, &platform->dev, leds.devt, NULL, DEVICE_NAME);
	if (IS_ERR(leds.device)) {
		ret = PTR_ERR(leds.device);
		leds.device = NULL;
		mutex_lock(&leds.lock);
		leds.available = false;
		mutex_unlock(&leds.lock);
		class_destroy(leds.class);
		goto err_cdev;
	}
	platform_set_drvdata(platform, &leds);
	dev_info(&platform->dev, "Registered /dev/%s using GPIO descriptors\n", DEVICE_NAME);
	return 0;

err_cdev:
	cdev_del(&leds.cdev);
err_unregister:
	unregister_chrdev_region(leds.devt, 1);
	return ret;
}

static void phonostethos_leds_remove(struct platform_device *platform)
{
	mutex_lock(&leds.lock);
	leds.available = false;
	set_led_mask(0);
	mutex_unlock(&leds.lock);
	cancel_delayed_work_sync(&leds.error_pulse_work);
	device_destroy(leds.class, leds.devt);
	class_destroy(leds.class);
	cdev_del(&leds.cdev);
	unregister_chrdev_region(leds.devt, 1);
	leds.device = NULL;
}

static const struct of_device_id phonostethos_leds_matches[] = {
	{ .compatible = "phonostethos,status-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, phonostethos_leds_matches);

static struct platform_driver phonostethos_leds_driver = {
	.probe = phonostethos_leds_probe,
	.remove = phonostethos_leds_remove,
	.driver = {
		.name = "phonostethos-status-leds",
		.of_match_table = phonostethos_leds_matches,
	},
};
module_platform_driver(phonostethos_leds_driver);
MODULE_DESCRIPTION("PhonoStethos GPIO-descriptor status LED driver");
MODULE_AUTHOR("PhonoStethos");
MODULE_LICENSE("GPL");
