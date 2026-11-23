/**
 * phonostethos_leds.c - implements the four-LED Linux character device with the classroom-style GPIO register mapping.
 *
 * CONNECTIONS: LedDriver.cpp opens /dev/phonostethos_leds and writes one binary mask byte. This kernel module validates bits 0..3, serializes writes and drives GPIO17/27/24/22. It does not create pulses, timers or feedback measurements.
 *
 * FUNCTIONS:
 * - DEFINE_MUTEX(): DEFINE MUTEX.
 * - set_gpio_function(): change only the selected GPIO's three function-select bits.
 * - set_led_mask(): map the four low mask bits to GPIO 17, 27, 24 and 22.
 * - led_open(): attach the GPIO mapping to the opened kernel file handle.
 * - led_release(): detach the mapping from this closing file handle.
 * - led_write(): copy and validate one user-space mask byte and update the GPIOs under a kernel mutex.
 * - led_init(): map GPIO registers, configure outputs and register the LED character device.
 * - class_destroy(): class destroy.
 * - cdev_del(): cdev del.
 * - unregister_chrdev_region(): unregister chrdev region.
 * - led_exit(): unregister the device, turn off LEDs and release the GPIO mapping.
 */

// Raspberry Pi 4 LED driver, following the classroom character-device example.
#include <linux/cdev.h>      // Character-device registration.
#include <linux/device.h>    // Device class and /dev node.
#include <linux/fs.h>        // struct file and file_operations.
#include <linux/io.h>        // ioremap, readl and writel.
#include <linux/module.h>    // Module load and unload.
#include <linux/mutex.h>     // Prevent simultaneous mask writes.
#include <linux/uaccess.h>   // Copy data safely from a user process.

#define DEVICE_NAME "phonostethos_leds" // Define DEVICE_NAME for the preprocessor.
#define CLASS_NAME "phonostethos" // Define CLASS_NAME for the preprocessor.
#define GPIO_BASE 0xFE200000 // BCM2711 GPIO base on Raspberry Pi 4.

// The layout matches the GPIO registers used in the professor's utils.h.
struct GpioRegisters { // Group the related fields in one plain data record.
    u32 GPFSEL[6]; // Three function bits per pin, ten pins per register.
    u32 reserved1; // Declare reserved1 for this operation.
    u32 GPSET[2];  // Write a 1 to make the selected output high.
    u32 reserved2; // Declare reserved2 for this operation.
    u32 GPCLR[2];  // Write a 1 to make the selected output low.
}; // End this block, declaration or argument list.

static struct GpioRegisters __iomem *gpio; // Complete this expression or declaration.
static dev_t device_number; // Declare device number for this operation.
static struct cdev led_cdev; // Complete this expression or declaration.
static struct class *led_class; // Complete this expression or declaration.
static const unsigned int led_pins[4] = {17, 27, 24, 22}; // Complete this expression or declaration.
static DEFINE_MUTEX(led_lock); // Serialize write() calls.

// Same calculation as SetGPIOFunction in the lecture.
static void set_gpio_function(unsigned int pin, unsigned int function) // Continue the expression or parameter list from the preceding line.
{ // Begin this local scope; scoped locks release when it ends.
    unsigned int register_index = pin / 10; // Complete this expression or declaration.
    unsigned int shift = (pin % 10) * 3; // Complete this expression or declaration.
/* API: writel
 * Write one 32-bit mapped GPIO register. GPSET/GPCLR use one bits to select which output pins change.
 */

    /* API: readl(address) and writel(value, address)
     * Read or write one 32-bit hardware register through its mapped address.
     * Clear only this pin's three function bits, then insert its new function.
     */
    u32 value = readl(&gpio->GPFSEL[register_index]); // Initialize value to readl(&gpio->GPFSEL[register_index]).
    value = value & ~(7U << shift); // Set value to value & ~(7U << shift).
    value = value | (function << shift); // Set value to value | (function << shift).
    writel(value, &gpio->GPFSEL[register_index]); // Call writel with the arguments shown here.
} // End this block, declaration or argument list.

static void set_led_mask(u8 mask) // Continue the expression or parameter list from the preceding line.
{ // Begin this local scope; scoped locks release when it ends.
    unsigned int index; // Complete this expression or declaration.
    for (index = 0; index < 4; index++) { // Visit the specified items/indexes in order.
        unsigned int pin = led_pins[index]; // Complete this expression or declaration.
        unsigned int gpio_bit = 1U << pin; // All selected pins are below GPIO32.
        if (mask & (1U << index)) { // Test the condition mask & (1U << index).
            writel(gpio_bit, &gpio->GPSET[0]); // Call writel with the arguments shown here.
        } else { // Follow the alternative path when the previous condition is false.
            writel(gpio_bit, &gpio->GPCLR[0]); // Call writel with the arguments shown here.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
} // End this block, declaration or argument list.

// open() connects this file handle with the mapped GPIO registers.
static int led_open(struct inode *inode, struct file *file) // Continue the expression or parameter list from the preceding line.
{ // Begin this local scope; scoped locks release when it ends.
    file->private_data = gpio; // Set file->private data to gpio.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

static int led_release(struct inode *inode, struct file *file) // Continue the expression or parameter list from the preceding line.
{ // Begin this local scope; scoped locks release when it ends.
    file->private_data = NULL; // Set file->private data to NULL.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

static ssize_t led_write(struct file *file, const char __user *buffer, // Pass this next argument/value to the call or initializer above.
                         size_t length, loff_t *offset) // Declare length for this operation.
{ // Begin this local scope; scoped locks release when it ends.
    u8 mask; // Declare mask for this operation.
    if (file->private_data == NULL) return -ENODEV; // Test the condition file->private_data == NULL. If true, return -enodev to the caller.
    if (length != 1) return -EINVAL; // Test the condition length != 1. If true, return -einval to the caller.

    /* API: copy_from_user(destination, user_address, byte_count)
     * The user buffer belongs to a process. Copy its single binary byte into
     * the kernel. A nonzero return means the copy failed.
     */
    if (copy_from_user(&mask, buffer, 1) != 0) return -EFAULT; // Test the condition copy_from_user(&mask, buffer, 1) != 0. If true, return -efault to the caller.
    if (mask > 0x0F) return -EINVAL; // Only four LED bits are defined.

    /* API: mutex_lock / mutex_unlock
     * Only one caller can change the complete LED mask at a time.
     */
    mutex_lock(&led_lock); // Call mutex_lock with the arguments shown here.
    set_led_mask(mask); // Map the four low mask bits to GPIO 17, 27, 24 and 22.
    mutex_unlock(&led_lock); // Call mutex_unlock with the arguments shown here.
    return 1; // One byte was accepted.
} // End this block, declaration or argument list.

// These callbacks have the signatures required by the Linux kernel.
static const struct file_operations led_operations = { // Continue the expression or parameter list from the preceding line.
    .owner = THIS_MODULE, // Set .owner to THIS_MODULE,.
    .open = led_open, // Set .open to led_open,.
    .release = led_release, // Set .release to led_release,.
    .write = led_write, // Set .write to led_write,.
    .llseek = noop_llseek, // Set .llseek to noop_llseek,.
}; // End this block, declaration or argument list.

static int __init led_init(void) // Continue the expression or parameter list from the preceding line.
{ // Begin this local scope; scoped locks release when it ends.
    int result; // Declare result for this operation.
    unsigned int index; // Complete this expression or declaration.
    struct device *device; // Group the related fields in one plain data record.

    /* API: ioremap(physical_address, byte_count)
     * Map the GPIO registers so the kernel can access them. NULL means failure.
     */
    gpio = ioremap(GPIO_BASE, sizeof(struct GpioRegisters)); // Set gpio to ioremap(GPIO_BASE, sizeof(struct GpioRegisters)).
    if (gpio == NULL) return -ENOMEM; // Test the condition gpio == NULL. If true, return -enomem to the caller.
    set_led_mask(0); // Map the four low mask bits to GPIO 17, 27, 24 and 22.
    for (index = 0; index < 4; index++) { // Visit the specified items/indexes in order.
        set_gpio_function(led_pins[index], 1); // 001 selects GPIO output.
    } // End this block, declaration or argument list.

    /* API: alloc_chrdev_region / cdev_init / cdev_add
     * Reserve a major/minor number, attach our callbacks, then register them.
     * A failed step jumps to cleanup for the steps that already succeeded.
     */
    result = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME); // Set result to alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME).
    if (result < 0) goto error_gpio; // Test the condition result < 0. If true, clean up only the resources acquired before this failed startup step.
    cdev_init(&led_cdev, &led_operations); // Call cdev_init with the arguments shown here.
    result = cdev_add(&led_cdev, device_number, 1); // Set result to cdev_add(&led_cdev, device_number, 1).
    if (result < 0) goto error_number; // Test the condition result < 0. If true, clean up only the resources acquired before this failed startup step.

    /* API: class_create / device_create
     * Publish the class and character device. devtmpfs/udev creates its node.
     * Our kernel uses class_create(name), unlike the older lecture kernel.
     */
    led_class = class_create(CLASS_NAME); // Set led class to class_create(CLASS_NAME).
    if (IS_ERR(led_class)) { // Test the condition IS_ERR(led_class).
        result = PTR_ERR(led_class); // Set result to PTR_ERR(led_class).
        goto error_cdev; // Clean up only the resources acquired before this failed startup step.
    } // End this block, declaration or argument list.
    device = device_create(led_class, NULL, device_number, NULL, DEVICE_NAME); // Set device to device_create(led_class, NULL, device_number, NULL, DEVICE_NAME).
    if (IS_ERR(device)) { // Test the condition IS_ERR(device).
        result = PTR_ERR(device); // Set result to PTR_ERR(device).
        goto error_class; // Clean up only the resources acquired before this failed startup step.
    } // End this block, declaration or argument list.
    pr_info("PhonoStethos: registered /dev/%s\n", DEVICE_NAME); // Call pr_info with the arguments shown here.
    return 0; // Return 0 to the caller.

error_class: // Cleanup entry used after the corresponding initialization step fails.
    /* API: class_destroy
     * Release a registered kernel device class after its device is removed, including failed-startup cleanup.
     */
    class_destroy(led_class); // Call class_destroy with the arguments shown here.
error_cdev: // Cleanup entry used after the corresponding initialization step fails.
    /* API: cdev_del
     * Unregister the character-device callbacks after users can no longer open the published device.
     */
    cdev_del(&led_cdev); // Call cdev_del with the arguments shown here.
error_number: // Cleanup entry used after the corresponding initialization step fails.
    /* API: unregister_chrdev_region
     * Return the major/minor range acquired during module initialization. Only release it after successful allocation.
     */
    unregister_chrdev_region(device_number, 1); // Call unregister_chrdev_region with the arguments shown here.
error_gpio: // Cleanup entry used after the corresponding initialization step fails.
    set_led_mask(0); // Map the four low mask bits to GPIO 17, 27, 24 and 22.
    for (index = 0; index < 4; index++) set_gpio_function(led_pins[index], 0); // Visit the specified items/indexes in order.
    iounmap(gpio); // Call iounmap with the arguments shown here.
    return result; // Return result to the caller.
} // End this block, declaration or argument list.

static void __exit led_exit(void) // Continue the expression or parameter list from the preceding line.
{ // Begin this local scope; scoped locks release when it ends.
    unsigned int index; // Complete this expression or declaration.
    // Reverse registration, turn LEDs off and return the pins to input.
    /* API: device_destroy
     * Remove the character-device node associated with this class and device number before releasing registration.
     */
    device_destroy(led_class, device_number); // Call device_destroy with the arguments shown here.
    cdev_del(&led_cdev); // Call cdev_del with the arguments shown here.
    class_destroy(led_class); // Call class_destroy with the arguments shown here.
    unregister_chrdev_region(device_number, 1); // Call unregister_chrdev_region with the arguments shown here.
    set_led_mask(0); // Map the four low mask bits to GPIO 17, 27, 24 and 22.
    for (index = 0; index < 4; index++) set_gpio_function(led_pins[index], 0); // Visit the specified items/indexes in order.

    /* API: iounmap(mapped_address)
     * Release the kernel mapping after the final GPIO access.
     */
    iounmap(gpio); // Call iounmap with the arguments shown here.
} // End this block, declaration or argument list.

module_init(led_init); // Register this kernel module's callback or descriptive metadata.
module_exit(led_exit); // Register this kernel module's callback or descriptive metadata.
MODULE_LICENSE("GPL"); // Register this kernel module's callback or descriptive metadata.
MODULE_DESCRIPTION("Raspberry Pi 4 four-LED mask character driver"); // Register this kernel module's callback or descriptive metadata.
MODULE_AUTHOR("PhonoStethos"); // Register this kernel module's callback or descriptive metadata.
