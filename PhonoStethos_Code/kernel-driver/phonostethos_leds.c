#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define DEVICE_NAME "phonostethos_leds"
#define CLASS_NAME "phonostethos"
#define GPIO_BASE 0xfe200000
#define GPIO_HEART 17
#define GPIO_LUNG 27
#define GPIO_GENERAL 24
#define GPIO_RECORDING 22
#define LED_HEART 0x01
#define LED_LUNG 0x02
#define LED_GENERAL 0x04
#define LED_RECORDING 0x08
#define LED_ALL (LED_HEART | LED_LUNG | LED_GENERAL | LED_RECORDING)
#define ERROR_PULSE 0xff

struct gpio_registers {
	u32 function_select[6];
	u32 reserved0;
	u32 set[2];
	u32 reserved1;
	u32 clear[2];
};

struct phonostethos_leds {
	void __iomem *gpio;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *device;
	struct mutex lock;
	struct delayed_work error_pulse_work;
	u8 cached_mask;
};

static struct phonostethos_leds leds;

static void set_gpio_output(unsigned int gpio)
{
	struct gpio_registers __iomem *registers = leds.gpio;
	const unsigned int index = gpio / 10;
	const unsigned int shift = (gpio % 10) * 3;
	u32 value = readl(&registers->function_select[index]);

	writel(BIT(gpio), &registers->clear[0]);
	value &= ~(0x7 << shift);
	value |= 0x1 << shift;
	writel(value, &registers->function_select[index]);
}

static void set_led_mask(u8 mask)
{
	struct gpio_registers __iomem *registers = leds.gpio;
	u32 set_mask = 0;
	u32 clear_mask = 0;

	if (mask & LED_HEART)
		set_mask |= BIT(GPIO_HEART);
	else
		clear_mask |= BIT(GPIO_HEART);
	if (mask & LED_LUNG)
		set_mask |= BIT(GPIO_LUNG);
	else
		clear_mask |= BIT(GPIO_LUNG);
	if (mask & LED_GENERAL)
		set_mask |= BIT(GPIO_GENERAL);
	else
		clear_mask |= BIT(GPIO_GENERAL);
	if (mask & LED_RECORDING)
		set_mask |= BIT(GPIO_RECORDING);
	else
		clear_mask |= BIT(GPIO_RECORDING);

	writel(set_mask, &registers->set[0]);
	writel(clear_mask, &registers->clear[0]);
}

static void restore_led_mask(struct work_struct *work)
{
	struct phonostethos_leds *device =
		container_of(to_delayed_work(work), struct phonostethos_leds,
			     error_pulse_work);

	mutex_lock(&device->lock);
	set_led_mask(device->cached_mask);
	mutex_unlock(&device->lock);
}

static int phonostethos_leds_open(struct inode *inode, struct file *file)
{
	file->private_data =
		container_of(inode->i_cdev, struct phonostethos_leds, cdev);
	return 0;
}

static ssize_t phonostethos_leds_write(struct file *file,
				       const char __user *buffer,
				       size_t length, loff_t *offset)
{
	struct phonostethos_leds *device = file->private_data;
	u8 command;

	if (length != sizeof(command))
		return -EINVAL;
	if (copy_from_user(&command, buffer, sizeof(command)))
		return -EFAULT;
	if (command != ERROR_PULSE && (command & ~LED_ALL))
		return -EINVAL;

	if (command != ERROR_PULSE)
		cancel_delayed_work_sync(&device->error_pulse_work);

	if (mutex_lock_interruptible(&device->lock))
		return -ERESTARTSYS;

	if (command == ERROR_PULSE) {
		set_led_mask(LED_ALL);
		mod_delayed_work(system_wq, &device->error_pulse_work,
				 msecs_to_jiffies(100));
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
	.write = phonostethos_leds_write,
	.llseek = noop_llseek,
};

static int __init phonostethos_leds_init(void)
{
	int ret;

	if (!of_machine_is_compatible("brcm,bcm2711")) {
		pr_err(DEVICE_NAME ": only Raspberry Pi 4 (BCM2711) is supported\n");
		return -ENODEV;
	}

	leds.gpio = ioremap(GPIO_BASE, sizeof(struct gpio_registers));
	if (!leds.gpio)
		return -ENOMEM;

	mutex_init(&leds.lock);
	INIT_DELAYED_WORK(&leds.error_pulse_work, restore_led_mask);

	ret = alloc_chrdev_region(&leds.devt, 0, 1, DEVICE_NAME);
	if (ret)
		goto err_unmap;

	cdev_init(&leds.cdev, &phonostethos_leds_fops);
	leds.cdev.owner = THIS_MODULE;
	ret = cdev_add(&leds.cdev, leds.devt, 1);
	if (ret)
		goto err_unregister;

	leds.class = class_create(CLASS_NAME);
	if (IS_ERR(leds.class)) {
		ret = PTR_ERR(leds.class);
		goto err_cdev;
	}

	leds.device = device_create(leds.class, NULL, leds.devt, NULL,
				    DEVICE_NAME);
	if (IS_ERR(leds.device)) {
		ret = PTR_ERR(leds.device);
		goto err_class;
	}

	set_gpio_output(GPIO_HEART);
	set_gpio_output(GPIO_LUNG);
	set_gpio_output(GPIO_GENERAL);
	set_gpio_output(GPIO_RECORDING);
	set_led_mask(0);

	pr_info(DEVICE_NAME ": registered /dev/%s\n", DEVICE_NAME);
	return 0;

err_class:
	class_destroy(leds.class);
err_cdev:
	cdev_del(&leds.cdev);
err_unregister:
	unregister_chrdev_region(leds.devt, 1);
err_unmap:
	iounmap(leds.gpio);
	return ret;
}

static void __exit phonostethos_leds_exit(void)
{
	cancel_delayed_work_sync(&leds.error_pulse_work);
	mutex_lock(&leds.lock);
	leds.cached_mask = 0;
	set_led_mask(0);
	mutex_unlock(&leds.lock);
	set_gpio_output(GPIO_HEART);
	set_gpio_output(GPIO_LUNG);
	set_gpio_output(GPIO_GENERAL);
	set_gpio_output(GPIO_RECORDING);
	iounmap(leds.gpio);
	device_destroy(leds.class, leds.devt);
	class_destroy(leds.class);
	cdev_del(&leds.cdev);
	unregister_chrdev_region(leds.devt, 1);
}

module_init(phonostethos_leds_init);
module_exit(phonostethos_leds_exit);

MODULE_DESCRIPTION("PhonoStethos Raspberry Pi 4 GPIO status LED driver");
MODULE_AUTHOR("PhonoStethos");
MODULE_LICENSE("GPL");
