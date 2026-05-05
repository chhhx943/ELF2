#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ioctl.h>

#define DEVICE_NAME "relay"
#define RELAY_IOC_MAGIC 'R'
#define SET_RELAY_ON_IO _IO(RELAY_IOC_MAGIC, 1)
#define SET_RELAY_OFF_IO _IO(RELAY_IOC_MAGIC, 0)

static int gpio = -1;



static int relay_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "Relay device opened\n");
    return 0;
}

static int relay_release(struct inode *inode,struct file *file){
    printk(KERN_INFO "Relay device release\n");
    return 0;
}

static long myrelay_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch(cmd) {
        case SET_RELAY_ON_IO:
            gpio_set_value(gpio, 1);
            printk(KERN_INFO "Relay turned ON\n");
            break;
        case SET_RELAY_OFF_IO:
            gpio_set_value(gpio, 0);
            printk(KERN_INFO "Relay turned OFF\n");
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = relay_open,
    .release = relay_release,
    .unlocked_ioctl = myrelay_ioctl,
};

static struct miscdevice relay_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &fops,
};

static int my_platform_probe(struct platform_device *pdev) {
    int ret;
    ret = misc_register(&relay_misc_device);
    if (ret) {
        printk(KERN_ALERT "Failed to register misc device\n");
        return ret;
    }
    gpio = of_get_named_gpio(pdev->dev.of_node, "relay-gpios", 0);
    if(!gpio_is_valid(gpio)) {
        printk(KERN_ALERT "Failed to get GPIO from device tree\n");
        misc_deregister(&relay_misc_device);
        return -EINVAL;
    }
    ret = gpio_request(gpio, "relay_gpio");
    if (ret < 0) {
        printk(KERN_ALERT "Failed to request GPIO pin %d\n", gpio);
        misc_deregister(&relay_misc_device);
        return ret;
    }
    ret = gpio_direction_output(gpio, 0);
    if (ret < 0) {
        printk(KERN_ALERT "Failed to set GPIO %d as output\n", gpio);
        gpio_free(gpio);
        misc_deregister(&relay_misc_device);
        return ret;
    }
     printk(KERN_INFO "Relay device probed successfully\n");
    return 0;
}

static int my_platform_remove(struct platform_device *pdev) {
    gpio_set_value(gpio, 0);
    gpio_free(gpio);
    misc_deregister(&relay_misc_device);
    printk(KERN_INFO "Relay device removed\n");
    return 0;
}

static const struct of_device_id my_of_ids[] = {
    { .compatible = "elfboard,gpio-relay", },
    { }
};
MODULE_DEVICE_TABLE(of, my_of_ids);

static struct platform_driver my_platform_driver = {
    .probe = my_platform_probe,
    .remove = my_platform_remove,
    .driver = {
        .name = "gpio-relay",
        .of_match_table = my_of_ids,
    },
};

module_platform_driver(my_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple relay character device");
