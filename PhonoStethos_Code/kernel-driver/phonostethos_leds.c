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
#define CLASS_NAME "phonostethos"
#define LED_HEART 0x01
#define LED_LUNG 0x02
#define LED_GENERAL 0x04
#define LED_RECORDING 0x08
#define LED_ALL (LED_HEART | LED_LUNG | LED_GENERAL | LED_RECORDING)
#define ERROR_PULSE 0xff

struct phonostethos_leds {
	struct device *dev;
	struct gpio_desc *heart;
	struct gpio_desc *lung;
	struct gpio_desc *general;
	struct gpio_desc *recording;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *char_device;
	struct mutex lock;
	struct delayed_work error_pulse_work;
	u8 cached_mask;
};

static void phonostethos_leds_set(struct phonostethos_leds *leds, u8 mask)
{
	gpiod_set_value_cansleep(leds->heart, !!(mask & LED_HEART));
	gpiod_set_value_cansleep(leds->lung, !!(mask & LED_LUNG));
	gpiod_set_value_cansleep(leds->general, !!(mask & LED_GENERAL));
	gpiod_set_value_cansleep(leds->recording, !!(mask & LED_RECORDING));
}

static void phonostethos_leds_restore_mask(struct work_struct *work)
{
	struct phonostethos_leds *leds =
		container_of(to_delayed_work(work), struct phonostethos_leds,
			     error_pulse_work);

	mutex_lock(&leds->lock);
	phonostethos_leds_set(leds, leds->cached_mask);
	mutex_unlock(&leds->lock);
}

static int phonostethos_leds_open(struct inode *inode, struct file *file)
{
	struct phonostethos_leds *leds =
		container_of(inode->i_cdev, struct phonostethos_leds, cdev);

	file->private_data = leds;
	return 0;
}

static ssize_t phonostethos_leds_write(struct file *file,
				       const char __user *buffer,
				       size_t length, loff_t *offset)
{
	struct phonostethos_leds *leds = file->private_data;
	u8 command;

	if (length != sizeof(command))
		return -EINVAL;
	if (copy_from_user(&command, buffer, sizeof(command)))
		return -EFAULT;
	if (command != ERROR_PULSE && (command & ~LED_ALL))
		return -EINVAL;

	if (command != ERROR_PULSE)
		cancel_delayed_work_sync(&leds->error_pulse_work);

	if (mutex_lock_interruptible(&leds->lock))
		return -ERESTARTSYS;

	if (command == ERROR_PULSE) {
		phonostethos_leds_set(leds, LED_ALL);
		mod_delayed_work(system_wq, &leds->error_pulse_work,
				 msecs_to_jiffies(100));
	} else {
		leds->cached_mask = command;
		phonostethos_leds_set(leds, command);
	}

	mutex_unlock(&leds->lock);
	return sizeof(command);
}

static int phonostethos_leds_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations phonostethos_leds_fops = {
	.owner = THIS_MODULE,
	.open = phonostethos_leds_open,
	.write = phonostethos_leds_write,
	.release = phonostethos_leds_release,
	.llseek = noop_llseek,
};

static int phonostethos_leds_probe(struct platform_device *pdev)
{
	struct phonostethos_leds *leds;
	int ret;

	leds = devm_kzalloc(&pdev->dev, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;
	leds->dev = &pdev->dev;
	mutex_init(&leds->lock);
	INIT_DELAYED_WORK(&leds->error_pulse_work,
			  phonostethos_leds_restore_mask);

	leds->heart = devm_gpiod_get(&pdev->dev, "heart", GPIOD_OUT_LOW);
	if (IS_ERR(leds->heart))
		return dev_err_probe(&pdev->dev, PTR_ERR(leds->heart),
				     "failed to acquire heart LED GPIO\n");
	leds->lung = devm_gpiod_get(&pdev->dev, "lung", GPIOD_OUT_LOW);
	if (IS_ERR(leds->lung))
		return dev_err_probe(&pdev->dev, PTR_ERR(leds->lung),
				     "failed to acquire lung LED GPIO\n");
	leds->recording = devm_gpiod_get(&pdev->dev, "recording",
					 GPIOD_OUT_LOW);
	if (IS_ERR(leds->recording))
		return dev_err_probe(&pdev->dev, PTR_ERR(leds->recording),
				     "failed to acquire recording LED GPIO\n");
	leds->general = devm_gpiod_get(&pdev->dev, "general", GPIOD_OUT_LOW);
	if (IS_ERR(leds->general))
		return dev_err_probe(&pdev->dev, PTR_ERR(leds->general),
				     "failed to acquire general LED GPIO\n");

	ret = alloc_chrdev_region(&leds->devt, 0, 1, DEVICE_NAME);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to allocate character device number\n");

	cdev_init(&leds->cdev, &phonostethos_leds_fops);
	leds->cdev.owner = THIS_MODULE;
	ret = cdev_add(&leds->cdev, leds->devt, 1);
	if (ret)
		goto err_unregister;

	leds->class = class_create(CLASS_NAME);
	if (IS_ERR(leds->class)) {
		ret = PTR_ERR(leds->class);
		goto err_cdev;
	}

	leds->char_device = device_create(leds->class, &pdev->dev, leds->devt,
					  leds, DEVICE_NAME);
	if (IS_ERR(leds->char_device)) {
		ret = PTR_ERR(leds->char_device);
		goto err_class;
	}

	platform_set_drvdata(pdev, leds);
	dev_info(&pdev->dev, "registered /dev/%s\n", DEVICE_NAME);
	return 0;

err_class:
	class_destroy(leds->class);
err_cdev:
	cdev_del(&leds->cdev);
err_unregister:
	unregister_chrdev_region(leds->devt, 1);
	return ret;
}

static void phonostethos_leds_remove(struct platform_device *pdev)
{
	struct phonostethos_leds *leds = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&leds->error_pulse_work);
	mutex_lock(&leds->lock);
	leds->cached_mask = 0;
	phonostethos_leds_set(leds, 0);
	mutex_unlock(&leds->lock);
	device_destroy(leds->class, leds->devt);
	class_destroy(leds->class);
	cdev_del(&leds->cdev);
	unregister_chrdev_region(leds->devt, 1);
}

static const struct of_device_id phonostethos_leds_of_match[] = {
	{ .compatible = "phonostethos,status-leds" },
	{}
};
MODULE_DEVICE_TABLE(of, phonostethos_leds_of_match);

static struct platform_driver phonostethos_leds_driver = {
	.probe = phonostethos_leds_probe,
	.remove = phonostethos_leds_remove,
	.driver = {
		.name = "phonostethos-leds",
		.of_match_table = phonostethos_leds_of_match,
	},
};
module_platform_driver(phonostethos_leds_driver);

MODULE_DESCRIPTION("PhonoStethos GPIO status LED character driver");
MODULE_AUTHOR("PhonoStethos");
MODULE_LICENSE("GPL");
