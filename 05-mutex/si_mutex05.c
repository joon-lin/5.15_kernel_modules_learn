#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "si_char_mutex"
#define CLASS_NAME "si_char_mutex_class"
#define BUFFER_SIZE 128

static dev_t device_number;
static struct cdev si_cdev;
static struct class *si_class;

static char device_buffer[BUFFER_SIZE] = "hello from mutex device\n";
static size_t device_size = sizeof("hello from mutex device\n") - 1;

/*
 * device_buffer 和 device_size 是共享数据：
 * 多个进程可能同时执行 read() 或 write()。
 *
 * DEFINE_MUTEX() 会声明并初始化一个 mutex。
 * 访问这两个共享变量前必须先拿到这把锁。
 */
static DEFINE_MUTEX(buffer_lock);

static int si_mutex_open(struct inode *inode, struct file *file)
{
	pr_info("si_mutex05: device opened\n");
	return 0;
}

static int si_mutex_release(struct inode *inode, struct file *file)
{
	pr_info("si_mutex05: device closed\n");
	return 0;
}

static ssize_t si_mutex_read(struct file *file, char __user *user_buffer,
				     size_t count, loff_t *offset)
{
	size_t available;
	ssize_t ret;

	if (*offset < 0)
		return -EINVAL;

	/*
	 * mutex_lock_interruptible() 可能被信号打断。
	 * 打断时返回 -ERESTARTSYS，不能继续访问共享缓冲区。
	 */
	if (mutex_lock_interruptible(&buffer_lock))
		return -ERESTARTSYS;

	/* 从这里开始，到 mutex_unlock() 之前都处于临界区。 */
	if (*offset >= device_size) {
		ret = 0; /* EOF */
		goto unlock;
	}

	available = device_size - *offset;
	if (count > available)
		count = available;

	if (copy_to_user(user_buffer, device_buffer + *offset, count)) {
		ret = -EFAULT;
		goto unlock;
	}

	*offset += count;
	ret = count;

unlock:
	/* 无论成功还是失败，都必须释放锁。 */
	mutex_unlock(&buffer_lock);
	return ret;
}

static ssize_t si_mutex_write(struct file *file,
				      const char __user *user_buffer,
				      size_t count, loff_t *offset)
{
	ssize_t ret;

	if (count >= BUFFER_SIZE)
		count = BUFFER_SIZE - 1;

	if (mutex_lock_interruptible(&buffer_lock))
		return -ERESTARTSYS;

	if (copy_from_user(device_buffer, user_buffer, count)) {
		ret = -EFAULT;
		goto unlock;
	}

	device_buffer[count] = '\0';
	device_size = count;
	*offset = 0;
	ret = count;

unlock:
	mutex_unlock(&buffer_lock);
	return ret;
}

static const struct file_operations si_mutex_fops = {
	.owner = THIS_MODULE,
	.open = si_mutex_open,
	.read = si_mutex_read,
	.write = si_mutex_write,
	.release = si_mutex_release,
};

static int __init si_mutex_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&si_cdev, &si_mutex_fops);
	si_cdev.owner = THIS_MODULE;

	ret = cdev_add(&si_cdev, device_number, 1);
	if (ret)
		goto unregister_region;

	si_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(si_class)) {
		ret = PTR_ERR(si_class);
		goto delete_cdev;
	}

	if (IS_ERR(device_create(si_class, NULL, device_number, NULL,
				 DEVICE_NAME))) {
		ret = -ENODEV;
		goto destroy_class;
	}

	pr_info("si_mutex05: registered /dev/%s major=%d minor=%d\n",
		DEVICE_NAME, MAJOR(device_number), MINOR(device_number));
	return 0;

destroy_class:
	class_destroy(si_class);
delete_cdev:
	cdev_del(&si_cdev);
unregister_region:
	unregister_chrdev_region(device_number, 1);
	return ret;
}

static void __exit si_mutex_exit(void)
{
	device_destroy(si_class, device_number);
	class_destroy(si_class);
	cdev_del(&si_cdev);
	unregister_chrdev_region(device_number, 1);
	pr_info("si_mutex05: unregistered\n");
}

module_init(si_mutex_init);
module_exit(si_mutex_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 05: mutex protected character device");
