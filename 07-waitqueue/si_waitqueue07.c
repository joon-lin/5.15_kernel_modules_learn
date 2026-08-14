#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define DEVICE_NAME "si_char_waitqueue"
#define CLASS_NAME "si_char_waitqueue_class"
#define BUFFER_SIZE 128

static dev_t device_number;
static struct cdev si_cdev;
static struct class *si_class;

static char device_buffer[BUFFER_SIZE];
static size_t device_size;
static bool data_ready;

/* 等待队列：没有数据时，read() 会把调用它的进程放到这里睡眠。 */
static DECLARE_WAIT_QUEUE_HEAD(read_queue);

/* 保护 device_buffer、device_size 和 data_ready。 */
static DEFINE_MUTEX(buffer_lock);

/*
 * ==================== wait queue API 速查 ====================
 *
 * 等待队列解决的是“条件不满足时，当前进程怎么办”：
 *
 *     条件满足：继续执行
 *     条件不满足：睡眠，让出 CPU，等待别人唤醒
 *
 * mutex 保护共享数据，wait queue 负责睡眠和唤醒；两者是不同的职责，
 * 经常一起使用。本课的 read_queue 就是等待“data_ready == true”。
 *
 * 一、创建和初始化
 *
 *   DECLARE_WAIT_QUEUE_HEAD(name);
 *       静态声明并初始化等待队列头。本课使用：
 *
 *       static DECLARE_WAIT_QUEUE_HEAD(read_queue);
 *
 *   wait_queue_head_t queue;
 *   init_waitqueue_head(&queue);
 *       运行时初始化动态分配或结构体成员中的等待队列头。
 *
 * 二、等待条件
 *
 *   wait_event(queue, condition);
 *       条件为假时睡眠；只会在条件变为真后返回。不可被普通信号打断。
 *
 *   wait_event_interruptible(queue, condition);
 *       本课使用的版本。条件为假时进入 TASK_INTERRUPTIBLE 睡眠：
 *
 *       返回 0              条件满足，正常返回
 *       返回 -ERESTARTSYS   被信号打断，没有等到条件
 *
 *       返回非 0 时，调用者应直接返回或处理错误，不要假设条件已经满足。
 *
 *   wait_event_killable(queue, condition);
 *       类似 interruptible，但只响应致命信号，例如 SIGKILL。
 *
 *   wait_event_timeout(queue, condition, timeout);
 *       最多等待 timeout 个 jiffies。返回 0 表示超时；非 0 表示条件
 *       满足，返回值通常是剩余的 jiffies。
 *
 *   wait_event_interruptible_timeout(queue, condition, timeout);
 *       可被信号打断，并且有超时。返回值可能是：
 *
 *       0                  超时
 *       -ERESTARTSYS       被信号打断
 *       大于 0             条件满足，返回剩余 jiffies
 *
 *   wait_event_hrtimeout(queue, condition, timeout);
 *   wait_event_interruptible_hrtimeout(queue, condition, timeout);
 *       使用 ktime_t 的高精度超时版本，返回 0、剩余时间或负错误码。
 *       普通驱动只有在确实需要高精度超时时才使用。
 *
 *   所有 wait_event 类宏都会反复检查 condition，不是“被唤醒一次就一定
 *   返回”。因此必须把真正的条件写进去，不能只等待一个无意义的通知。
 *
 * 三、唤醒等待者
 *
 *   wake_up(&queue);
 *       唤醒使用 TASK_UNINTERRUPTIBLE 等待的任务。
 *
 *   wake_up_interruptible(&queue);
 *       本课使用的版本，唤醒 TASK_INTERRUPTIBLE 等待的任务。
 *
 *   wake_up_all(&queue);
 *   wake_up_interruptible_all(&queue);
 *       唤醒队列中所有符合状态的任务。普通 wake_up() 通常只唤醒一个
 *       合适的独占等待者，具体行为取决于等待者的设置。
 *
 *   wake_up_nr(&queue, nr);
 *   wake_up_interruptible_nr(&queue, nr);
 *       最多唤醒 nr 个等待者。
 *
 *   唤醒的标准写法是：先更新共享条件，再调用 wake_up：
 *
 *       mutex_lock(&buffer_lock);
 *       data_ready = true;
 *       mutex_unlock(&buffer_lock);
 *       wake_up_interruptible(&read_queue);
 *
 *       不能只 wake_up() 而不改变 condition，否则被唤醒的任务检查到
 *       条件仍为假后会继续睡眠。
 *
 * 四、手动等待接口（高级用法）
 *
 *   DEFINE_WAIT(wait);
 *   prepare_to_wait(&queue, &wait, state);
 *   prepare_to_wait_exclusive(&queue, &wait, state);
 *   finish_wait(&queue, &wait);
 *
 *   这些接口让驱动手动控制“加入等待队列、设置任务状态、schedule、
 *   从队列移除”的全过程。wait_event 宏已经替我们正确处理了这些步骤，
 *   初学和普通驱动优先使用 wait_event_*，不要手写这套流程。
 *
 *   add_wait_queue(&queue, &wait);
 *   add_wait_queue_exclusive(&queue, &wait);
 *   remove_wait_queue(&queue, &wait);
 *       手动增删等待项，通常只在实现特殊等待逻辑时使用。
 *
 *   waitqueue_active(&queue);
 *       检查队列是否可能有等待者。它只能作为性能优化提示，不能用来
 *       代替 wake_up，也不能作为正确性判断。
 *
 * 五、必须遵守的模式
 *
 *   生产者（本课的 write）：
 *
 *       1. 获取 mutex
 *       2. 修改数据和等待条件
 *       3. 释放 mutex
 *       4. wake_up_interruptible()
 *
 *   消费者（本课的 read）：
 *
 *       1. wait_event_interruptible(queue, condition)
 *       2. 检查返回值，处理信号
 *       3. 获取 mutex，再次确认条件
 *       4. 读取并清理数据
 *       5. 释放 mutex
 *
 *   为什么要“唤醒后再次确认条件”？因为可能有多个读进程同时被唤醒，
 *   其中一个已经把数据取走，另一个醒来时条件可能又变成 false；此外，
 *   虚假唤醒和多个生产者也要求代码始终以条件为准，而不是以“醒了”为准。
 *
 * 六、上下文限制
 *
 *   wait_event_* 会让当前任务睡眠，只能在进程上下文使用，不能在硬中断、
 *   软中断或持有 raw_spinlock_t 时调用。wake_up_* 本身可以在中断上下文
 *   调用，所以中断处理函数可以更新合适的状态后唤醒进程。
 *
 * ==================== wait queue API 速查结束 ====================
 */

static int si_wait_open(struct inode *inode, struct file *file)
{
	pr_info("si_waitqueue07: device opened\n");
	return 0;
}

static int si_wait_release(struct inode *inode, struct file *file)
{
	pr_info("si_waitqueue07: device closed\n");
	return 0;
}

static ssize_t si_wait_read(struct file *file, char __user *user_buffer,
				    size_t count, loff_t *offset)
{
	ssize_t ret;

	for (;;) {
	/*
	 * 没有数据时，这里会睡眠并让出 CPU。write() 设置 data_ready 后
	 * 调用 wake_up_interruptible()，本调用才会继续。
	 */
		pr_info("si_waitqueue07: read waiting, data_ready=%d\n",
			READ_ONCE(data_ready));
		ret = wait_event_interruptible(read_queue, READ_ONCE(data_ready));
		if (ret)
			return -ERESTARTSYS;
		pr_info("si_waitqueue07: read woke up\n");

		/* 醒来后仍然要拿锁并再次检查条件，不能只相信“我被唤醒了”。 */
		if (mutex_lock_interruptible(&buffer_lock))
			return -ERESTARTSYS;

		if (!data_ready) {
			/* 另一个读进程可能已经取走数据；释放锁后重新等待。 */
			mutex_unlock(&buffer_lock);
			continue;
		}

		if (count < device_size) {
			/* 缓冲区太小，保留数据，用户下次用更大的 buffer 重试。 */
			ret = -EMSGSIZE;
			goto unlock;
		}

		if (copy_to_user(user_buffer, device_buffer, device_size)) {
			ret = -EFAULT;
			goto unlock;
		}

		ret = device_size;
		*offset = 0;
		device_size = 0;
		WRITE_ONCE(data_ready, false);

unlock:
		mutex_unlock(&buffer_lock);
		return ret;
	}
}

static ssize_t si_wait_write(struct file *file,
				     const char __user *user_buffer,
				     size_t count, loff_t *offset)
{
	if (count == 0)
		return 0;

	if (count >= BUFFER_SIZE)
		count = BUFFER_SIZE - 1;

	/* mutex 允许睡眠，因此 copy_from_user() 可以放在锁内。 */
	if (mutex_lock_interruptible(&buffer_lock))
		return -ERESTARTSYS;

	if (copy_from_user(device_buffer, user_buffer, count)) {
		mutex_unlock(&buffer_lock);
		return -EFAULT;
	}

	device_size = count;
	WRITE_ONCE(data_ready, true);
	*offset = 0;
	mutex_unlock(&buffer_lock);

	/* 条件已经变为真，再唤醒等待 read() 的进程。 */
	pr_info("si_waitqueue07: write stored %zu bytes, waking readers\n", count);
	wake_up_interruptible(&read_queue);
	return count;
}

static const struct file_operations si_wait_fops = {
	.owner = THIS_MODULE,
	.open = si_wait_open,
	.read = si_wait_read,
	.write = si_wait_write,
	.release = si_wait_release,
};

static int __init si_wait_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&si_cdev, &si_wait_fops);
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

	pr_info("si_waitqueue07: registered /dev/%s major=%d minor=%d\n",
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

static void __exit si_wait_exit(void)
{
	device_destroy(si_class, device_number);
	class_destroy(si_class);
	cdev_del(&si_cdev);
	unregister_chrdev_region(device_number, 1);
	pr_info("si_waitqueue07: unregistered\n");
}

module_init(si_wait_init);
module_exit(si_wait_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 07: wait queue and blocking character device");
