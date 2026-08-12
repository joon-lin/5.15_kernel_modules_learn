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
 * ==================== mutex API 速查 ====================
 *
 * mutex 用来保护“进程上下文”中的共享数据。和 raw_spinlock_t 不同，
 * mutex 获取失败时，当前任务会进入睡眠，把 CPU 让给其他任务。
 * 因此 mutex 临界区里可以调用可能睡眠的函数，例如 copy_to_user()、
 * copy_from_user()、kmalloc(..., GFP_KERNEL) 或 wait_event()。
 *
 * 一、声明和初始化
 *
 *   DEFINE_MUTEX(lock);
 *       静态声明并初始化一把 mutex。本课使用这种方式：
 *
 *       static DEFINE_MUTEX(buffer_lock);
 *
 *   struct mutex lock;
 *   mutex_init(&lock);
 *       运行时初始化动态分配或结构体成员中的 mutex。
 *       mutex_init() 只能对未初始化、未持有的 mutex 调用。
 *
 *   mutex_destroy(&lock);
 *       销毁动态初始化的 mutex，通常在释放外层对象前调用。
 *       对 DEFINE_MUTEX() 的静态 mutex 一般不需要调用；在当前配置下，
 *       非调试版本的 mutex_destroy() 甚至可能是空操作。
 *
 * 二、获取 mutex
 *
 *   mutex_lock(&lock);
 *       阻塞等待直到拿到锁。返回值是 void，没有错误返回。
 *       等待期间忽略普通信号；只能在进程上下文使用，可能睡眠。
 *
 *   mutex_lock_interruptible(&lock);
 *       本课使用的版本。等待期间可以被信号打断：
 *
 *       返回 0              成功拿到锁
 *       返回 -ERESTARTSYS   被信号打断，没有拿到锁
 *
 *       返回非 0 时不能执行 mutex_unlock()，因为当前任务并没有拿到锁。
 *
 *   mutex_lock_killable(&lock);
 *       和 mutex_lock_interruptible() 类似，但只响应致命信号，常见返回
 *       值为 -EINTR。适合不希望普通信号打断、但仍希望任务能被 SIGKILL
 *       等致命信号唤醒的场景。
 *
 *   mutex_lock_io(&lock);
 *       和 mutex_lock() 的等待语义基本相同，但等待锁时把当前任务计入
 *       I/O wait 状态，供调度器统计。普通驱动很少需要它。
 *
 *   mutex_trylock(&lock);
 *       非阻塞尝试获取 mutex，不会睡眠：
 *
 *       返回 1   成功拿到锁
 *       返回 0   锁正在被别人持有
 *
 *       trylock 失败时不能调用 mutex_unlock()。它虽然不等待，但 mutex
 *       仍然要求由进程上下文中的同一个任务获取和释放，所以不能把它
 *       当成中断处理函数里的自旋锁替代品。
 *
 * 三、释放和状态查询
 *
 *   mutex_unlock(&lock);
 *       释放当前任务已经持有的 mutex。必须和成功的加锁操作配对，不能
 *       由另一个任务代替释放，也不能重复解锁。
 *
 *   mutex_is_locked(&lock);
 *       查询 mutex 当前是否被持有，返回 true 或 false。它主要用于调试
 *       和断言，不能用来代替真正的加锁；“先查询、后加锁”不是原子操作。
 *
 * 四、lockdep 的嵌套版本
 *
 *   mutex_lock_nested(&lock, subclass);
 *   mutex_lock_interruptible_nested(&lock, subclass);
 *   mutex_lock_killable_nested(&lock, subclass);
 *   mutex_lock_io_nested(&lock, subclass);
 *   mutex_lock_nest_lock(&lock, nest_lock);
 *
 *   这些接口用于告诉 lockdep：当前存在已知的锁嵌套层级或锁依赖关系，
 *   主要用于复杂子系统的死锁检测。普通驱动通常使用不带 _nested 的
 *   简单版本，不要为了“看起来完整”而随意填写 subclass。
 *
 * 五、和引用计数配合的接口
 *
 *   atomic_dec_and_mutex_lock(&count, &lock);
 *
 *   原子地减少 count；当 count 减到 0 时再获取 lock，成功时返回 true。
 *   常用于对象引用计数归零、准备执行最后清理的场景。本课程暂不使用。
 *
 * 六、必须遵守的规则
 *
 *   1. mutex 可能睡眠，不能在硬中断、软中断、tasklet 中调用：
 *
 *          mutex_lock()       错误
 *          mutex_trylock()    也不能因此当作中断锁使用
 *          mutex_unlock()     也必须由原来持锁的进程执行
 *
 *      中断上下文需要使用合适的 spinlock/raw_spinlock_t，或者把工作
 *      延后到 workqueue、内核线程等进程上下文。
 *
 *   2. mutex 不允许递归获取：同一个任务重复 mutex_lock() 会死锁。
 *
 *   3. 所有成功的加锁路径都必须有且只有一个 mutex_unlock()，错误路径
 *      建议使用 goto unlock 统一释放，避免遗漏。
 *
 *   4. 不要把用户指针、可能无效的地址或耗时很长的无关操作放进临界区；
 *      虽然 mutex 允许睡眠，但锁持有太久仍会阻塞其他任务。
 *
 * 七、当前 GJ3 RT 内核的说明
 *
 *   当前内核 CONFIG_PREEMPT_RT=y。mutex 的底层实现使用 rtmutex，仍然
 *   保持“竞争时可以睡眠”的 mutex 语义，并支持实时任务需要的优先级
 *   继承。不要因为底层实现不同就把 mutex 当成 raw_spinlock 使用。
 *
 * 本课的 device_buffer/device_size 是共享数据：多个进程可能同时执行
 * read() 或 write()。访问它们前先获取 buffer_lock，完成后释放。
 * 当前 read()/write() 使用 mutex_lock_interruptible()，所以收到可处理
 * 信号时会返回错误，而不会在没有拿到锁的情况下继续访问共享数据。
 *
 * ==================== mutex API 速查结束 ====================
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
