#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define DEVICE_NAME "os_lab_char"
#define CLASS_NAME "os_lab"
#define PROC_NAME "os_lab_info"

static dev_t dev_no;
static struct cdev os_cdev;
static struct class *os_class;
static struct device *os_device;
static char *device_buffer;
static unsigned long capacity = 4096;
static size_t data_len;
static DEFINE_MUTEX(buffer_lock);
static atomic64_t open_count = ATOMIC64_INIT(0);
static atomic64_t read_count = ATOMIC64_INIT(0);
static atomic64_t write_count = ATOMIC64_INIT(0);

module_param(capacity, ulong, 0444);
MODULE_PARM_DESC(capacity, "Character device buffer capacity in bytes");

static int os_open(struct inode *inode, struct file *file) {
    atomic64_inc(&open_count);
    return 0;
}

static ssize_t os_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos) {
    ssize_t copied;

    if (mutex_lock_interruptible(&buffer_lock)) {
        return -ERESTARTSYS;
    }
    if (*ppos >= data_len) {
        mutex_unlock(&buffer_lock);
        return 0;
    }
    if (count > data_len - *ppos) {
        count = data_len - *ppos;
    }
    if (copy_to_user(user_buf, device_buffer + *ppos, count)) {
        mutex_unlock(&buffer_lock);
        return -EFAULT;
    }
    *ppos += count;
    copied = (ssize_t)count;
    mutex_unlock(&buffer_lock);
    atomic64_inc(&read_count);
    return copied;
}

static ssize_t os_write(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos) {
    if (count > capacity) {
        return -ENOSPC;
    }
    if (mutex_lock_interruptible(&buffer_lock)) {
        return -ERESTARTSYS;
    }
    if (copy_from_user(device_buffer, user_buf, count)) {
        mutex_unlock(&buffer_lock);
        return -EFAULT;
    }
    data_len = count;
    *ppos = count;
    mutex_unlock(&buffer_lock);
    atomic64_inc(&write_count);
    return (ssize_t)count;
}

static const struct file_operations os_fops = {
    .owner = THIS_MODULE,
    .open = os_open,
    .read = os_read,
    .write = os_write,
    .llseek = no_llseek,
};

static int proc_show(struct seq_file *m, void *v) {
    size_t len_snapshot;

    mutex_lock(&buffer_lock);
    len_snapshot = data_len;
    mutex_unlock(&buffer_lock);

    seq_printf(m, "device=%s\n", DEVICE_NAME);
    seq_printf(m, "major=%d minor=%d\n", MAJOR(dev_no), MINOR(dev_no));
    seq_printf(m, "capacity=%lu length=%zu\n", capacity, len_snapshot);
    seq_printf(m, "opens=%lld reads=%lld writes=%lld\n",
               atomic64_read(&open_count),
               atomic64_read(&read_count),
               atomic64_read(&write_count));
    return 0;
}

static int proc_open_fn(struct inode *inode, struct file *file) {
    return single_open(file, proc_show, NULL);
}

static const struct proc_ops os_proc_ops = {
    .proc_open = proc_open_fn,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf) {
    size_t len_snapshot;

    mutex_lock(&buffer_lock);
    len_snapshot = data_len;
    mutex_unlock(&buffer_lock);

    return sysfs_emit(buf,
                      "capacity=%lu length=%zu opens=%lld reads=%lld writes=%lld\n",
                      capacity,
                      len_snapshot,
                      atomic64_read(&open_count),
                      atomic64_read(&read_count),
                      atomic64_read(&write_count));
}
static DEVICE_ATTR_RO(stats);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    if (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y') {
        mutex_lock(&buffer_lock);
        memset(device_buffer, 0, capacity);
        data_len = 0;
        mutex_unlock(&buffer_lock);
        atomic64_set(&open_count, 0);
        atomic64_set(&read_count, 0);
        atomic64_set(&write_count, 0);
    }
    return count;
}
static DEVICE_ATTR_WO(reset);

static int __init os_driver_init(void) {
    int ret;

    if (capacity == 0 || capacity > 1024 * 1024) {
        pr_err("os_lab_driver: capacity must be 1..1048576\n");
        return -EINVAL;
    }
    device_buffer = kzalloc(capacity, GFP_KERNEL);
    if (!device_buffer) {
        return -ENOMEM;
    }

    ret = alloc_chrdev_region(&dev_no, 0, 1, DEVICE_NAME);
    if (ret) {
        goto err_buffer;
    }

    cdev_init(&os_cdev, &os_fops);
    os_cdev.owner = THIS_MODULE;
    ret = cdev_add(&os_cdev, dev_no, 1);
    if (ret) {
        goto err_unregister;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    os_class = class_create(CLASS_NAME);
#else
    os_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(os_class)) {
        ret = PTR_ERR(os_class);
        goto err_cdev;
    }

    os_device = device_create(os_class, NULL, dev_no, NULL, DEVICE_NAME);
    if (IS_ERR(os_device)) {
        ret = PTR_ERR(os_device);
        goto err_class;
    }

    ret = device_create_file(os_device, &dev_attr_stats);
    if (ret) {
        goto err_device;
    }
    ret = device_create_file(os_device, &dev_attr_reset);
    if (ret) {
        goto err_stats;
    }
    if (!proc_create(PROC_NAME, 0444, NULL, &os_proc_ops)) {
        ret = -ENOMEM;
        goto err_reset;
    }

    pr_info("os_lab_driver: loaded /dev/%s major=%d minor=%d capacity=%lu\n",
            DEVICE_NAME, MAJOR(dev_no), MINOR(dev_no), capacity);
    return 0;

err_reset:
    device_remove_file(os_device, &dev_attr_reset);
err_stats:
    device_remove_file(os_device, &dev_attr_stats);
err_device:
    device_destroy(os_class, dev_no);
err_class:
    class_destroy(os_class);
err_cdev:
    cdev_del(&os_cdev);
err_unregister:
    unregister_chrdev_region(dev_no, 1);
err_buffer:
    kfree(device_buffer);
    return ret;
}

static void __exit os_driver_exit(void) {
    remove_proc_entry(PROC_NAME, NULL);
    device_remove_file(os_device, &dev_attr_reset);
    device_remove_file(os_device, &dev_attr_stats);
    device_destroy(os_class, dev_no);
    class_destroy(os_class);
    cdev_del(&os_cdev);
    unregister_chrdev_region(dev_no, 1);
    kfree(device_buffer);
    pr_info("os_lab_driver: unloaded\n");
}

module_init(os_driver_init);
module_exit(os_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS course design");
MODULE_DESCRIPTION("Character device with /proc and /sys interfaces for OS course design");
