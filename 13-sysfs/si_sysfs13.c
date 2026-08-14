/*
 * ============================================================================
 * Lesson 13：sysfs 教学
 * ============================================================================
 *
 * 一、sysfs 是什么？
 *
 * sysfs 是内核提供的虚拟文件系统，通常挂载在 /sys。它不是磁盘上的
 * 普通文件，而是把内核中的设备、驱动、总线和属性，展示成用户空间
 * 可以访问的目录和文件。
 *
 * 例如，网卡的物理链路状态可以这样查看：
 *
 *     cat /sys/class/net/eth0/carrier
 *
 * 二、这一课最终创建什么？
 *
 * 加载模块后会创建：
 *
 *     /sys/class/si_lesson13/demo/message
 *     /sys/class/si_lesson13/demo/write_count
 *
 *     cat message
 *         调用 message_show()
 *     echo hello > message
 *         调用 message_store()
 *     cat write_count
 *         调用 write_count_show()
 *
 * 三、class、device、attribute 的关系
 *
 *     class_create()
 *         创建 /sys/class/si_lesson13/
 *
 *     device_create()
 *         创建 /sys/class/si_lesson13/demo/
 *
 *     DEVICE_ATTR_RW(message)
 *         定义 message 文件及其 show/store 回调
 *
 *     sysfs_create_group()
 *         把多个 attribute 一次性挂到 demo 设备目录
 *
 * 最终层级可以理解为：
 *
 *     class
 *       └── device
 *             ├── message
 *             └── write_count
 *
 * 四、show/store 回调的参数
 *
 *     show(struct device *dev, ..., char *buf)
 *         用户读取 sysfs 文件时调用。驱动把内容写入 buf，返回字节数。
 *
 *     store(struct device *dev, ..., const char *buf, size_t count)
 *         用户写入 sysfs 文件时调用。buf 已经是内核缓冲区，成功时返回
 *         count，表示本次写入的全部字节已经处理。
 *
 * sysfs 回调收到的 buf 不属于用户地址空间，所以这里不需要：
 *
 *     copy_to_user()
 *     copy_from_user()
 *
 * 五、为什么使用 sysfs？
 *
 * sysfs 适合暴露简单的状态和配置，例如：
 *
 *     enable、mode、speed、status、count
 *
 * 它不适合传输大量数据。大量数据应使用字符设备 read/write、ioctl，
 * 调试信息通常使用 debugfs。
 *
 * 六、并发和生命周期
 *
 * show() 和 store() 可能被不同进程同时调用，因此本例用 mutex 保护
 * message 和 write_count。
 *
 * 模块卸载时必须按照相反顺序清理：
 *
 *     sysfs_remove_group()
 *     device_destroy()
 *     class_destroy()
 *     kfree()
 *
 * 否则用户空间可能继续访问已经释放的内核对象。
 * ============================================================================
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define CLASS_NAME "si_lesson13"
#define DEVICE_NAME "demo"
#define MESSAGE_SIZE 64

struct si_sysfs_data {
	struct class *class;
	struct device *device;
	struct mutex lock;
	char message[MESSAGE_SIZE];
	unsigned int write_count;
};

static struct si_sysfs_data *si_data;

/*
 * 读取：
 *
 *     cat /sys/class/si_lesson13/demo/message
 *
 * buf 是 sysfs 提供的内核缓冲区，不需要 copy_to_user()。
 */
static ssize_t message_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct si_sysfs_data *data = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&data->lock);
	ret = sysfs_emit(buf, "%s\n", data->message);
	mutex_unlock(&data->lock);

	return ret;
}

/*
 * 写入：
 *
 *     echo hello > /sys/class/si_lesson13/demo/message
 *
 * buf 和 count 已经由 sysfs 传入内核，不需要 copy_from_user()。
 * sysfs 属性的 store 回调成功时应返回 count。
 */
static ssize_t message_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct si_sysfs_data *data = dev_get_drvdata(dev);
	size_t len = min_t(size_t, count, MESSAGE_SIZE - 1);

	mutex_lock(&data->lock);
	memcpy(data->message, buf, len);

	/* echo 通常会附带换行，这里把它从属性值中去掉。 */
	if (len > 0 && data->message[len - 1] == '\n')
		len--;
	data->message[len] = '\0';
	data->write_count++;
	mutex_unlock(&data->lock);

	return count;
}

static ssize_t write_count_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct si_sysfs_data *data = dev_get_drvdata(dev);
	unsigned int count;

	mutex_lock(&data->lock);
	count = data->write_count;
	mutex_unlock(&data->lock);

	return sysfs_emit(buf, "%u\n", count);
}

/* 根据同名的 *_show()/ *_store() 函数生成 sysfs 属性。 */
static DEVICE_ATTR_RW(message);
static DEVICE_ATTR_RO(write_count);

/* 一个属性组对应一个 sysfs 目录下的多个属性文件。 */
static struct attribute *si_sysfs_attrs[] = {
	&dev_attr_message.attr,
	&dev_attr_write_count.attr,
	NULL,
};

static const struct attribute_group si_sysfs_attr_group = {
	.attrs = si_sysfs_attrs,
};

static int __init si_sysfs_init(void)
{
	int ret;

	si_data = kzalloc(sizeof(*si_data), GFP_KERNEL);
	if (!si_data)
		return -ENOMEM;

	mutex_init(&si_data->lock);
	strscpy(si_data->message, "hello sysfs", sizeof(si_data->message));

	/* class_create() 对应 /sys/class/si_lesson13/。 */
	si_data->class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(si_data->class)) {
		ret = PTR_ERR(si_data->class);
		goto err_free_data;
	}

	/* device_create() 对应 /sys/class/si_lesson13/demo/。 */
	si_data->device = device_create(si_data->class, NULL, 0, si_data,
					DEVICE_NAME);
	if (IS_ERR(si_data->device)) {
		ret = PTR_ERR(si_data->device);
		goto err_destroy_class;
	}

	ret = sysfs_create_group(&si_data->device->kobj,
					 &si_sysfs_attr_group);
	if (ret)
		goto err_destroy_device;

	pr_info("si_sysfs13: created /sys/class/%s/%s/\n",
		CLASS_NAME, DEVICE_NAME);
	return 0;

err_destroy_device:
	device_destroy(si_data->class, 0);
err_destroy_class:
	class_destroy(si_data->class);
err_free_data:
	kfree(si_data);
	si_data = NULL;
	return ret;
}

static void __exit si_sysfs_exit(void)
{
	sysfs_remove_group(&si_data->device->kobj, &si_sysfs_attr_group);
	device_destroy(si_data->class, 0);
	class_destroy(si_data->class);
	kfree(si_data);
	si_data = NULL;

	pr_info("si_sysfs13: removed\n");
}

module_init(si_sysfs_init);
module_exit(si_sysfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 13: sysfs attributes");
