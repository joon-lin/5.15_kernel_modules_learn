#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>

/*
 * 设备节点的名字。
 * device_create() 成功后，通常会出现：
 *
 *     /dev/si_char
 */
#define DEVICE_NAME "si_char"
#define CLASS_NAME "si_char_class"

/* 本课只使用一个固定大小的内核缓冲区。 */
#define BUFFER_SIZE 128

/*
 * dev_t 同时保存主设备号 major 和次设备号 minor。
 * alloc_chrdev_region() 会动态分配设备号，避免和系统已有设备冲突。
 */
static dev_t device_number;

/* cdev 是内核中代表“字符设备”的对象。 */
static struct cdev si_cdev;

/* class 用于在 /sys/class/ 下组织设备，并配合 udev 创建 /dev 节点。 */
static struct class *si_class;

/*
 * 这是设备实际保存数据的地方。
 * 用户通过 write() 写入，通过 read() 读出。
 * 为了突出字符设备流程，本课暂时不处理并发访问；后续再加入 mutex。
 */
static char device_buffer[BUFFER_SIZE] = "hello from si_char\n";
static size_t device_size = sizeof("hello from si_char\n") - 1;

/*
 * 用户空间执行 open("/dev/si_char") 时会调用这里。
 * 当前示例不需要为每个打开的文件保存额外状态。
 */
static int si_char_open(struct inode *inode, struct file *file)
{
	pr_info("si_char: device opened\n");
	return 0;
}

/*
 * 用户空间关闭文件时会调用这里，例如 cat 命令结束时。
 */
static int si_char_release(struct inode *inode, struct file *file)
{
	pr_info("si_char: device closed\n");
	return 0;
}

/*
 * 用户空间读取设备时调用这里，例如：
 *
 *     cat /dev/si_char
 *
 * user_buffer 是用户空间地址，不能直接解引用，必须使用 copy_to_user()。
 *
 * offset 是 VFS 传进来的文件偏移量指针，实际指向 file->f_pos。
 * 用户态只写 read(fd, buffer, count)，这个 offset 由内核根据 fd 自动维护；
 * 第一次 read() 通常从 0 开始，读完后增加；
 * 当 offset 到达 device_size 时返回 0，表示 EOF，cat 就会结束。
 */
static ssize_t si_char_read(struct file *file, char __user *user_buffer,
				 size_t count, loff_t *offset)
{
	size_t available;
	pr_info("si_char: read request count=%zu offset=%lld\n",
		count, *offset);

	if (*offset < 0)
		return -EINVAL;

	/* 已经读到缓冲区末尾，告诉用户空间没有更多数据。 */
	if (*offset >= device_size) {
		pr_info("si_char: read returned=0 (EOF) offset=%lld\n", *offset);
		return 0;
	}

	/* 本次最多只能读取还未读出的数据。 */
	available = device_size - *offset;
	if (count > available)
		count = available;

	/*
	 * 从内核空间复制到用户空间。
	 * 返回非零表示有部分数据没有复制成功。
	 */
	if (copy_to_user(user_buffer, device_buffer + *offset, count))
		return -EFAULT;

	/* 更新文件偏移量，下一次 read() 从后面继续读取。 */
	*offset += count;
	pr_info("si_char: read returned=%zu new_offset=%lld\n",
		count, *offset);
	return count;
}

/*
 * 用户空间写入设备时调用这里，例如：
 *
 *     echo "hello gj3" > /dev/si_char
 *
 * 这里把新数据覆盖到内核缓冲区中。
 * offset 同样由 VFS 自动传入，不是用户态 write() 显式传入的参数。
 */
static ssize_t si_char_write(struct file *file, const char __user *user_buffer,
				  size_t count, loff_t *offset)
{
	pr_info("si_char: write request count=%zu offset=%lld\n",
		count, *offset);

	/* 预留一个字节存放字符串结尾的 '\0'。 */
	if (count >= BUFFER_SIZE)
		count = BUFFER_SIZE - 1;

	/* 从用户空间复制到内核空间。 */
	if (copy_from_user(device_buffer, user_buffer, count))
		return -EFAULT;

	/* 让缓冲区成为一个合法的 C 字符串。 */
	device_buffer[count] = '\0';
	device_size = count;

	/* 下一次读取从新数据的开头开始。 */
	*offset = 0;
	pr_info("si_char: write stored=%zu new_offset=%lld\n",
		count, *offset);
	return count;
}

/*
 * file_operations 是字符设备和用户空间系统调用之间的“函数表”。
 * 用户空间的操作会映射到这里：
 *
 * open("/dev/si_char")  -> si_char_open()
 * read(fd, ...)         -> si_char_read()
 * write(fd, ...)        -> si_char_write()
 * close(fd)             -> si_char_release()
 */
static const struct file_operations si_char_fops = {
	.owner = THIS_MODULE,
	.open = si_char_open,
	.read = si_char_read,
	.write = si_char_write,
	.release = si_char_release,
};

/*
 * 模块加载时初始化字符设备，并创建 /dev/si_char。
 * 初始化过程必须按照“申请资源”的顺序执行；
 * 中途失败时，要反向释放已经申请成功的资源。
 */
static int __init si_char_init(void)
{
	int ret;

	/* 申请一个字符设备号，数量为 1 个。 */
	ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	/* 把 file_operations 绑定到 cdev 对象。 */
	cdev_init(&si_cdev, &si_char_fops);
	si_cdev.owner = THIS_MODULE;

	/* 把 cdev 注册到内核。 */
	ret = cdev_add(&si_cdev, device_number, 1);
	if (ret)
		goto unregister_region;

	/* 创建 /sys/class/si_char_class/，供设备模型管理。 */
	si_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(si_class)) {
		ret = PTR_ERR(si_class);
		goto delete_cdev;
	}

	/* 创建设备对象，通常会触发 udev 创建 /dev/si_char。 */
	if (IS_ERR(device_create(si_class, NULL, device_number, NULL,
				 DEVICE_NAME))) {
		ret = -ENODEV;
		goto destroy_class;
	}

	pr_info("si_char: registered major=%d minor=%d\n",
		MAJOR(device_number), MINOR(device_number));
	return 0;

/* 错误处理：按照申请资源的相反顺序释放。 */
destroy_class:
	class_destroy(si_class);
delete_cdev:
	cdev_del(&si_cdev);
unregister_region:
	unregister_chrdev_region(device_number, 1);
	return ret;
}

/*
 * 模块卸载时释放所有初始化阶段申请的资源。
 * 如果忘记释放，重复 insmod/rmmod 可能造成资源泄漏或设备号冲突。
 */
static void __exit si_char_exit(void)
{
	/* 删除 /dev/si_char 对应的设备对象。 */
	device_destroy(si_class, device_number);

	/* 删除 class。 */
	class_destroy(si_class);

	/* 从内核注销字符设备。 */
	cdev_del(&si_cdev);

	/* 释放动态申请的 major/minor 设备号。 */
	unregister_chrdev_region(device_number, 1);
	pr_info("si_char: unregistered\n");
}

module_init(si_char_init);
module_exit(si_char_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 04: simple character device");
