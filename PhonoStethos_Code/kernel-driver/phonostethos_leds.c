// Raspberry Pi 4 LED driver, following the classroom character-device example.
#include <linux/cdev.h>      // Character-device registration.
#include <linux/device.h>    // Device class and /dev node.
#include <linux/fs.h>        // struct file and file_operations.
#include <linux/io.h>        // ioremap, readl and writel.
#include <linux/module.h>    // Module load and unload.
#include <linux/mutex.h>     // Prevent simultaneous mask writes.
#include <linux/uaccess.h>   // Copy data safely from a user process.

#define DEVICE_NAME "phonostethos_leds"
#define CLASS_NAME "phonostethos"
#define GPIO_BASE 0xFE200000 // BCM2711 GPIO base on Raspberry Pi 4.

// The layout matches the GPIO registers used in the professor's utils.h.
struct GpioRegisters {
    u32 GPFSEL[6]; // Three function bits per pin, ten pins per register.
    u32 reserved1;
    u32 GPSET[2];  // Write a 1 to make the selected output high.
    u32 reserved2;
    u32 GPCLR[2];  // Write a 1 to make the selected output low.
};

static struct GpioRegisters __iomem *gpio;
static dev_t device_number;
static struct cdev led_cdev;
static struct class *led_class;
static const unsigned int led_pins[4] = {17, 27, 24, 22};
static DEFINE_MUTEX(led_lock); // Serialize write() calls.

// Same calculation as SetGPIOFunction in the lecture.
static void set_gpio_function(unsigned int pin, unsigned int function)
{
    unsigned int register_index = pin / 10;
    unsigned int shift = (pin % 10) * 3;

    /* API: readl(address) and writel(value, address)
     * Read or write one 32-bit hardware register through its mapped address.
     * Clear only this pin's three function bits, then insert its new function.
     */
    u32 value = readl(&gpio->GPFSEL[register_index]);
    value = value & ~(7U << shift);
    value = value | (function << shift);
    writel(value, &gpio->GPFSEL[register_index]);
}

static void set_led_mask(u8 mask)
{
    unsigned int index;
    for (index = 0; index < 4; index++) {
        unsigned int pin = led_pins[index];
        unsigned int gpio_bit = 1U << pin; // All selected pins are below GPIO32.
        if (mask & (1U << index)) {
            writel(gpio_bit, &gpio->GPSET[0]);
        } else {
            writel(gpio_bit, &gpio->GPCLR[0]);
        }
    }
}

// open() connects this file handle with the mapped GPIO registers.
static int led_open(struct inode *inode, struct file *file)
{
    file->private_data = gpio;
    return 0;
}

static int led_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static ssize_t led_write(struct file *file, const char __user *buffer,
                         size_t length, loff_t *offset)
{
    u8 mask;
    if (file->private_data == NULL) return -ENODEV;
    if (length != 1) return -EINVAL;

    /* API: copy_from_user(destination, user_address, byte_count)
     * The user buffer belongs to a process. Copy its single binary byte into
     * the kernel. A nonzero return means the copy failed.
     */
    if (copy_from_user(&mask, buffer, 1) != 0) return -EFAULT;
    if (mask > 0x0F) return -EINVAL; // Only four LED bits are defined.

    /* API: mutex_lock / mutex_unlock
     * Only one caller can change the complete LED mask at a time.
     */
    mutex_lock(&led_lock);
    set_led_mask(mask);
    mutex_unlock(&led_lock);
    return 1; // One byte was accepted.
}

// These callbacks have the signatures required by the Linux kernel.
static const struct file_operations led_operations = {
    .owner = THIS_MODULE,
    .open = led_open,
    .release = led_release,
    .write = led_write,
    .llseek = noop_llseek,
};

static int __init led_init(void)
{
    int result;
    unsigned int index;
    struct device *device;

    /* API: ioremap(physical_address, byte_count)
     * Map the GPIO registers so the kernel can access them. NULL means failure.
     */
    gpio = ioremap(GPIO_BASE, sizeof(struct GpioRegisters));
    if (gpio == NULL) return -ENOMEM;
    set_led_mask(0);
    for (index = 0; index < 4; index++) {
        set_gpio_function(led_pins[index], 1); // 001 selects GPIO output.
    }

    /* API: alloc_chrdev_region / cdev_init / cdev_add
     * Reserve a major/minor number, attach our callbacks, then register them.
     * A failed step jumps to cleanup for the steps that already succeeded.
     */
    result = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
    if (result < 0) goto error_gpio;
    cdev_init(&led_cdev, &led_operations);
    result = cdev_add(&led_cdev, device_number, 1);
    if (result < 0) goto error_number;

    /* API: class_create / device_create
     * Publish the class and character device. devtmpfs/udev creates its node.
     * Our kernel uses class_create(name), unlike the older lecture kernel.
     */
    led_class = class_create(CLASS_NAME);
    if (IS_ERR(led_class)) {
        result = PTR_ERR(led_class);
        goto error_cdev;
    }
    device = device_create(led_class, NULL, device_number, NULL, DEVICE_NAME);
    if (IS_ERR(device)) {
        result = PTR_ERR(device);
        goto error_class;
    }
    pr_info("PhonoStethos: registered /dev/%s\n", DEVICE_NAME);
    return 0;

error_class:
    class_destroy(led_class);
error_cdev:
    cdev_del(&led_cdev);
error_number:
    unregister_chrdev_region(device_number, 1);
error_gpio:
    set_led_mask(0);
    for (index = 0; index < 4; index++) set_gpio_function(led_pins[index], 0);
    iounmap(gpio);
    return result;
}

static void __exit led_exit(void)
{
    unsigned int index;
    // Reverse registration, turn LEDs off and return the pins to input.
    device_destroy(led_class, device_number);
    cdev_del(&led_cdev);
    class_destroy(led_class);
    unregister_chrdev_region(device_number, 1);
    set_led_mask(0);
    for (index = 0; index < 4; index++) set_gpio_function(led_pins[index], 0);

    /* API: iounmap(mapped_address)
     * Release the kernel mapping after the final GPIO access.
     */
    iounmap(gpio);
}

module_init(led_init);
module_exit(led_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Raspberry Pi 4 four-LED mask character driver");
MODULE_AUTHOR("PhonoStethos");
