#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "si_char_spinlock"
#define CLASS_NAME "si_char_spinlock_class"
#define BUFFER_SIZE 128

static dev_t device_number;
static struct cdev si_cdev;
static struct class *si_class;

static char device_buffer[BUFFER_SIZE] = "hello from spinlock device\n";
static size_t device_size = sizeof("hello from spinlock device\n") - 1;

/*
 * ==================== spinlock API 速查 ====================
 *
 * 声明和初始化：
 *   DEFINE_SPINLOCK(lock);       静态定义并初始化普通 spinlock_t
 *   spinlock_t lock;             结构体成员/动态对象
 *   spin_lock_init(&lock);       初始化普通 spinlock_t
 *
 * 普通锁：
 *   spin_lock(&lock);            等待并加锁
 *   spin_unlock(&lock);          解锁
 *   spin_trylock(&lock);         尝试加锁，成功返回 1，失败返回 0
 *
 * 硬中断相关（非 RT 内核的传统语义）：
 *   spin_lock_irq(&lock);        加锁并关闭本地硬中断
 *   spin_unlock_irq(&lock);      解锁并打开本地硬中断
 *   spin_lock_irqsave(&lock, flags);
 *                                保存中断状态、加锁
 *   spin_unlock_irqrestore(&lock, flags);
 *                                解锁并恢复原中断状态
 *   spin_trylock_irq(&lock);     不等待的 irq 版本
 *   spin_trylock_irqsave(&lock, flags);
 *                                不等待的 irqsave 版本
 *
 * 软中断相关：
 *   spin_lock_bh(&lock);         禁止本地 bottom half 后加锁
 *   spin_unlock_bh(&lock);       解锁并恢复 bottom half
 *   spin_trylock_bh(&lock);      不等待的 bh 版本
 *
 * 状态/调试：
 *   spin_is_locked(&lock);       查询是否被锁住
 *   spin_is_contended(&lock);    查询是否有竞争
 *   assert_spin_locked(&lock);   调试断言：当前应已持有锁
 *
 * 锁嵌套（主要给 lockdep 使用，初学阶段通常不用）：
 *   spin_lock_nested(&lock, subclass);
 *   spin_lock_nest_lock(&lock, nest_lock);
 *   spin_lock_irqsave_nested(&lock, flags, subclass);
 *
 * 原始自旋锁：
 *   DEFINE_RAW_SPINLOCK(lock);   静态定义并初始化 raw_spinlock_t
 *   raw_spinlock_t lock;         结构体成员/动态对象
 *   raw_spin_lock_init(&lock);   初始化 raw_spinlock_t
 *   raw_spin_lock(), raw_spin_unlock(),
 *   raw_spin_lock_irqsave(), raw_spin_unlock_irqrestore(),
 *   raw_spin_lock_bh(), raw_spin_unlock_bh()。
 * raw_spinlock_t 是底层原始自旋锁，不要和普通 spinlock_t 混用。
 * 不要直接调用 _raw_* 或 __raw_*，它们是内部实现。
 *
 * ==================== 当前 RT 内核的实际映射 ====================
 *
 * 本内核 CONFIG_PREEMPT_RT=y，include/linux/spinlock_rt.h 中：
 *   spin_lock_init()          -> rt_mutex_base_init()
 *   spin_lock()               -> rt_spin_lock()
 *   spin_unlock()             -> rt_spin_unlock()
 *   spin_lock_irqsave()       -> flags = 0; spin_lock()
 *   spin_unlock_irqrestore()  -> rt_spin_unlock()
 *   spin_lock_irq()           -> rt_spin_lock()
 *   spin_lock_bh()            -> local_bh_disable(); rt_spin_lock()
 *   spin_unlock_bh()          -> rt_spin_unlock(); local_bh_enable()
 *   spin_trylock()            -> rt_spin_trylock()
 *
 * 所以在 RT 内核中，spin_lock_irqsave() 仍然保留这个 API 的配对形式，
 * 但 flags 不再保存/恢复硬中断状态，而是被置为 0；它也不等同于传统
 * 非 RT 内核中的“关闭本地硬中断 + 忙等自旋”。真正绝对不能睡眠的底层
 * 锁是 raw_spinlock_t。
 *
 * 本课下面实际使用 raw_spinlock_t，让 RT 内核也保持传统的硬自旋锁语义。
 * raw_spinlock_t 只能保护极短、绝对不能睡眠的临界区。
 * 本课只在访问 device_buffer/device_size 时持有这把锁。
 */
static raw_spinlock_t buffer_lock;

static int si_spin_open(struct inode *inode, struct file *file)
{
	pr_info("si_spin06: device opened\n");
	return 0;
}

static int si_spin_release(struct inode *inode, struct file *file)
{
	pr_info("si_spin06: device closed\n");
	return 0;
}

static ssize_t si_spin_read(struct file *file, char __user *user_buffer,
				    size_t count, loff_t *offset)
{
	char temp[BUFFER_SIZE];
	size_t available;
	size_t copy_count;
	unsigned long flags;

	if (*offset < 0)
		return -EINVAL;

	/*
	 * 先把共享数据复制到本地数组，再释放自旋锁。
	 * 这里只使用 memcpy()，因为两边都是内核地址。
	 */
	raw_spin_lock_irqsave(&buffer_lock, flags);

	if (*offset >= device_size) {
		raw_spin_unlock_irqrestore(&buffer_lock, flags);
		return 0;
	}

	available = device_size - *offset;
	copy_count = count > available ? available : count;
	memcpy(temp, device_buffer + *offset, copy_count);
	*offset += copy_count;

	raw_spin_unlock_irqrestore(&buffer_lock, flags);

	/*
	 * copy_to_user() 可能睡眠，所以必须放在自旋锁外面。
	 */
	if (copy_to_user(user_buffer, temp, copy_count))
		return -EFAULT;

	return copy_count;
}

static ssize_t si_spin_write(struct file *file,
				     const char __user *user_buffer,
				     size_t count, loff_t *offset)
{
	char temp[BUFFER_SIZE];
	unsigned long flags;

	if (count >= BUFFER_SIZE)
		count = BUFFER_SIZE - 1;

	/*
	 * copy_from_user() 可能睡眠，所以先复制到本地数组。
	 * 这一步不能放在 raw_spin_lock_irqsave() 和 unlock 之间。
	 */
	if (copy_from_user(temp, user_buffer, count))
		return -EFAULT;

	/* 临界区很短：只更新共享缓冲区和长度。 */
	raw_spin_lock_irqsave(&buffer_lock, flags);
	memcpy(device_buffer, temp, count);
	device_buffer[count] = '\0';
	device_size = count;
	*offset = 0;
	raw_spin_unlock_irqrestore(&buffer_lock, flags);

	return count;
}

static const struct file_operations si_spin_fops = {
	.owner = THIS_MODULE,
	.open = si_spin_open,
	.read = si_spin_read,
	.write = si_spin_write,
	.release = si_spin_release,
};

static int __init si_spin_init(void)
{
	int ret;

	raw_spin_lock_init(&buffer_lock);

	ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&si_cdev, &si_spin_fops);
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

	pr_info("si_spin06: registered /dev/%s major=%d minor=%d\n",
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

static void __exit si_spin_exit(void)
{
	device_destroy(si_class, device_number);
	class_destroy(si_class);
	cdev_del(&si_cdev);
	unregister_chrdev_region(device_number, 1);
	pr_info("si_spin06: unregistered\n");
}

module_init(si_spin_init);
module_exit(si_spin_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 06: spinlock protected character device");
