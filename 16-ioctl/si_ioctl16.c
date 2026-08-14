/*
 * ============================================================================
 * Lesson 16：字符设备 ioctl 教学
 * ============================================================================
 *
 * 一、ioctl 是什么？
 *
 * ioctl（input/output control）是字符设备提供的“控制命令”接口。
 * read()/write() 适合连续字节流；ioctl() 适合明确的控制操作，例如：
 *
 *     设置一个整数
 *     读取一个状态
 *     传递一个结构体
 *     启动或停止某个硬件功能
 *
 * 用户态调用：
 *
 *     ioctl(fd, SI_IOCTL16_SET_VALUE, &value);
 *
 * 内核最终进入：
 *
 *     si_ioctl16_unlocked_ioctl(file, cmd, arg)
 *
 * 二、cmd 和 arg 分别是什么？
 *
 *     cmd：命令编号，包含方向、类型、序号和参数大小；
 *     arg：用户态传入的参数地址，内核中表现为 unsigned long。
 *
 * arg 不是可以直接解引用的内核指针：
 *
 *     *(u32 *)arg = value;       // 错误
 *
 * 必须使用：
 *
 *     copy_from_user()          // 用户空间 → 内核空间
 *     copy_to_user()            // 内核空间 → 用户空间
 *
 * 三、_IO/_IOR/_IOW/_IOWR
 *
 *     _IO(type, nr)              没有数据，只执行命令
 *     _IOR(type, nr, data_type)  用户从内核读取数据
 *     _IOW(type, nr, data_type)  用户向内核写入数据
 *     _IOWR(type, nr, data_type) 用户和内核双向传递数据
 *
 * 方向是从用户空间视角看的。比如：
 *
 *     _IOW(..., __u32)
 *
 * 表示用户把一个 `__u32` 写给驱动，而不是驱动写给用户。
 *
 * 四、ioctl 命令为什么要放在公共头文件？
 *
 * 用户程序和内核模块必须使用完全相同的命令编号和结构体布局。因此
 * `si_ioctl16_uapi.h` 同时被两边包含，它就是这个设备的用户态 ABI。
 * 结构体使用 `__u32` 等固定宽度类型，避免不同架构下大小变化。
 *
 * 五、本课的四个命令
 *
 *     SI_IOCTL16_SET_VALUE
 *         用户传入一个整数，驱动保存到 value。
 *
 *     SI_IOCTL16_GET_VALUE
 *         驱动把 value 复制回用户空间。
 *
 *     SI_IOCTL16_SET_DATA
 *         用户传入 value + message 结构体。
 *
 *     SI_IOCTL16_GET_DATA
 *         驱动把完整结构体复制回用户空间。
 *
 * 六、为什么 ioctl 中可以使用 mutex？
 *
 * ioctl 是由用户进程通过系统调用进入的，属于进程上下文，可以睡眠。
 * 因此本课用 mutex 保护驱动状态；copy_to_user()/copy_from_user() 也可能
 * 睡眠，不能放在硬中断上半部中。
 *
 * 七、标准处理流程
 *
 *     1. 检查 _IOC_TYPE(cmd) 和 _IOC_NR(cmd)；
 *     2. 把用户数据 copy_from_user() 到内核局部变量；
 *     3. 加锁修改或读取驱动状态；
 *     4. 解锁；
 *     5. 如果是读取命令，copy_to_user() 返回用户空间；
 *     6. 成功返回 0，失败返回负的 errno。
 *
 * 八、为什么不用 ioctl 返回数据？
 *
 * ioctl 的返回值通常只表示成功或错误：
 *
 *     0       成功
 *     -EFAULT 用户地址不可访问
 *     -EINVAL 命令或参数无效
 *     -ENOTTY 当前设备不支持该命令
 *
 * 真正的数据通过 arg 指向的用户缓冲区传递。
 * ============================================================================
 */

#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "si_ioctl16_uapi.h"

#define DRIVER_NAME "si_ioctl16"
#define CLASS_NAME "si_ioctl16"
#define DEVICE_NAME "si_ioctl16"

struct si_ioctl16_device {
	dev_t devno;
	struct cdev cdev;
	struct class *class;
	struct device *device;
	struct mutex lock;
	__u32 value;
	char message[SI_IOCTL16_MESSAGE_SIZE];
};

static struct si_ioctl16_device *si_dev;

static int si_ioctl16_open(struct inode *inode, struct file *file)
{
	file->private_data = si_dev;
	pr_info(DRIVER_NAME ": open\n");
	return 0;
}

static int si_ioctl16_release(struct inode *inode, struct file *file)
{
	pr_info(DRIVER_NAME ": release\n");
	return 0;
}

static long si_ioctl16_unlocked_ioctl(struct file *file,
					      unsigned int cmd,
					      unsigned long arg)
{
	struct si_ioctl16_device *dev = file->private_data;
	void __user *user_arg = (void __user *)arg;
	struct si_ioctl16_data data;
	__u32 value;

	if (_IOC_TYPE(cmd) != SI_IOCTL16_MAGIC)
		return -ENOTTY;

	if (_IOC_NR(cmd) < 1 || _IOC_NR(cmd) > 4)
		return -ENOTTY;

	pr_info(DRIVER_NAME ": ioctl cmd=0x%x nr=%u dir=0x%x size=%u\n",
		cmd, _IOC_NR(cmd), _IOC_DIR(cmd), _IOC_SIZE(cmd));

	switch (cmd) {
	case SI_IOCTL16_SET_VALUE:
		if (copy_from_user(&value, user_arg, sizeof(value)))
			return -EFAULT;

		mutex_lock(&dev->lock);
		dev->value = value;
		mutex_unlock(&dev->lock);
		pr_info(DRIVER_NAME ": SET_VALUE value=%u\n", value);
		return 0;

	case SI_IOCTL16_GET_VALUE:
		mutex_lock(&dev->lock);
		value = dev->value;
		mutex_unlock(&dev->lock);

		if (copy_to_user(user_arg, &value, sizeof(value)))
			return -EFAULT;
		pr_info(DRIVER_NAME ": GET_VALUE value=%u\n", value);
		return 0;

	case SI_IOCTL16_SET_DATA:
		if (copy_from_user(&data, user_arg, sizeof(data)))
			return -EFAULT;

		/* 确保用户传入的字符数组即使没有 '\0' 也能安全打印。 */
		data.message[SI_IOCTL16_MESSAGE_SIZE - 1] = '\0';
		mutex_lock(&dev->lock);
		dev->value = data.value;
		strscpy(dev->message, data.message, sizeof(dev->message));
		mutex_unlock(&dev->lock);
		pr_info(DRIVER_NAME ": SET_DATA value=%u message=\"%s\"\n",
			data.value, data.message);
		return 0;

	case SI_IOCTL16_GET_DATA:
		mutex_lock(&dev->lock);
		data.value = dev->value;
		strscpy(data.message, dev->message, sizeof(data.message));
		mutex_unlock(&dev->lock);

		if (copy_to_user(user_arg, &data, sizeof(data)))
			return -EFAULT;
		pr_info(DRIVER_NAME ": GET_DATA value=%u message=\"%s\"\n",
			data.value, data.message);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations si_ioctl16_fops = {
	.owner = THIS_MODULE,
	.open = si_ioctl16_open,
	.release = si_ioctl16_release,
	.unlocked_ioctl = si_ioctl16_unlocked_ioctl,
};

static int __init si_ioctl16_init(void)
{
	int ret;

	si_dev = kzalloc(sizeof(*si_dev), GFP_KERNEL);
	if (!si_dev)
		return -ENOMEM;

	mutex_init(&si_dev->lock);
	si_dev->value = 0;
	strscpy(si_dev->message, "hello ioctl", sizeof(si_dev->message));

	ret = alloc_chrdev_region(&si_dev->devno, 0, 1, DRIVER_NAME);
	if (ret)
		goto err_free;

	cdev_init(&si_dev->cdev, &si_ioctl16_fops);
	si_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&si_dev->cdev, si_dev->devno, 1);
	if (ret)
		goto err_unregister;

	si_dev->class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(si_dev->class)) {
		ret = PTR_ERR(si_dev->class);
		goto err_cdev_del;
	}

	si_dev->device = device_create(si_dev->class, NULL, si_dev->devno,
				       NULL, DEVICE_NAME);
	if (IS_ERR(si_dev->device)) {
		ret = PTR_ERR(si_dev->device);
		goto err_class_destroy;
	}

	pr_info(DRIVER_NAME ": registered /dev/%s major=%d minor=%d\n",
		DEVICE_NAME, MAJOR(si_dev->devno), MINOR(si_dev->devno));
	return 0;

err_class_destroy:
	class_destroy(si_dev->class);
err_cdev_del:
	cdev_del(&si_dev->cdev);
err_unregister:
	unregister_chrdev_region(si_dev->devno, 1);
err_free:
	kfree(si_dev);
	si_dev = NULL;
	return ret;
}

static void __exit si_ioctl16_exit(void)
{
	device_destroy(si_dev->class, si_dev->devno);
	class_destroy(si_dev->class);
	cdev_del(&si_dev->cdev);
	unregister_chrdev_region(si_dev->devno, 1);
	kfree(si_dev);
	si_dev = NULL;
	pr_info(DRIVER_NAME ": unregistered\n");
}

module_init(si_ioctl16_init);
module_exit(si_ioctl16_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 16: ioctl user/kernel control interface");
