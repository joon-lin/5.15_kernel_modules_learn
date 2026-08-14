#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define DEVICE_NAME "si_char_poll"
#define CLASS_NAME "si_char_poll_class"
#define BUFFER_SIZE 128

static dev_t device_number;
static struct cdev si_cdev;
static struct class *si_class;

static char device_buffer[BUFFER_SIZE];
static size_t device_size;
static bool data_ready;

/* poll() 和阻塞式 read() 都等待这条队列。 */
static DECLARE_WAIT_QUEUE_HEAD(read_queue);

/* 保护缓冲区、长度和 data_ready。 */
static DEFINE_MUTEX(buffer_lock);

/*
 * ==================== poll API 速查 ====================
 *
 * 07 课的 wait queue 解决“一个 read() 没数据时睡眠”的问题。
 * poll 把它再向前扩展一步：一个用户程序可以同时等待多个 fd，
 * 设备准备好后再决定读取哪个 fd。
 *
 * 一、驱动侧的 .poll 回调
 *
 *   static __poll_t driver_poll(struct file *file,
 *                               struct poll_table_struct *wait);
 *
 *   .poll = driver_poll;
 *
 *   poll_wait(file, &read_queue, wait);
 *       把当前 poll() 调用者注册到 read_queue。这个函数本身不睡眠，
 *       只是建立“以后有人 wake_up 时通知我”的关系。
 *
 *   返回值是事件位掩码：
 *
 *   POLLIN     / POLLRDNORM  有普通数据可读
 *   POLLPRI                   有高优先级数据可读
 *   POLLOUT    / POLLWRNORM  当前可以写入
 *   POLLERR                   发生错误
 *   POLLHUP                   设备挂断或对端关闭
 *
 *   本课的 .poll：
 *
 *       poll_wait(file, &read_queue, wait);
 *       if (READ_ONCE(data_ready))
 *               mask |= POLLIN | POLLRDNORM;
 *       mask |= POLLOUT | POLLWRNORM;
 *       return mask;
 *
 *   poll_wait() 只负责注册等待队列，真正“现在是否可读”必须由驱动
 *   自己返回掩码。不能只调用 poll_wait() 却永远返回 0。
 *
 * 二、驱动侧的唤醒逻辑
 *
 *   write() 修改条件后必须唤醒等待者：
 *
 *       mutex_lock(&buffer_lock);
 *       写入 device_buffer;
 *       WRITE_ONCE(data_ready, true);
 *       mutex_unlock(&buffer_lock);
 *       wake_up_interruptible(&read_queue);
 *
 *   poll() 被唤醒后会再次调用驱动的 .poll 回调。如果回调返回 POLLIN，
 *   用户态 poll() 才会报告“这个 fd 已经可读”。因此：
 *
 *       wake_up()       只是通知“状态可能改变了”
 *       .poll 返回掩码  才是告诉用户“具体哪些事件已经就绪”
 *
 * 三、用户态 poll()
 *
 *   struct pollfd fds[2];
 *   fds[0].fd = device_fd;
 *   fds[0].events = POLLIN;
 *   ret = poll(fds, 1, timeout_ms);
 *
 *   返回值：
 *
 *       > 0       有一个或多个 fd 发生事件
 *       0        timeout_ms 到期，没有事件
 *       -1       出错；errno == EINTR 表示被信号打断
 *
 *   返回后检查：
 *
 *       fds[0].revents & POLLIN
 *
 *   poll() 返回“可读”只代表现在读不会因为没有数据而阻塞，仍然要
 *   继续调用 read() 取得数据。poll() 不会把数据复制到用户缓冲区。
 *
 * 四、非阻塞读
 *
 *   int fd = open("/dev/si_char_poll", O_RDWR | O_NONBLOCK);
 *
 *   如果没有数据：
 *
 *       read() 返回 -1，errno == EAGAIN 或 EWOULDBLOCK
 *
 *   常见事件循环是：
 *
 *       poll(&fds, 1, -1);
 *       if (fds.revents & POLLIN)
 *               read(fd, buffer, size);
 *
 *   O_NONBLOCK 让 read() 自己不睡眠；poll() 负责等待事件。两者经常
 *   配合使用，但不是同一个概念。
 *
 * 五、select()
 *
 *   fd_set readfds;
 *   FD_ZERO(&readfds);
 *   FD_SET(fd, &readfds);
 *   select(fd + 1, &readfds, NULL, NULL, &timeout);
 *
 *   select() 也会使用驱动的 .poll 回调。返回后使用 FD_ISSET(fd, ...)
 *   检查事件。它的 fd 数量受 FD_SETSIZE 限制，并且每次调用都要重新
 *   准备 fd_set；新代码通常更倾向使用 poll 或 epoll。
 *
 * 六、epoll()
 *
 *   int epfd = epoll_create1(0);
 *   epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
 *   epoll_wait(epfd, events, maxevents, timeout);
 *
 *   epoll 仍然依赖驱动的 .poll 和等待队列，但适合同时管理大量 fd。
 *   EPOLLIN、EPOLLOUT、EPOLLERR、EPOLLHUP 与驱动返回的 POLL* 事件
 *   对应。epoll 的细节放在后续课程，本课先理解驱动接口。
 *
 * 七、阻塞式 read、poll 和非阻塞 read 的关系
 *
 *   阻塞 read：
 *       没数据时，read() 自己在 wait queue 上睡眠。
 *
 *   poll + 非阻塞 read：
 *       poll() 在 wait queue 上等待；就绪后，read() 立即取数据。
 *
 *   直接非阻塞 read：
 *       没数据立即返回 -EAGAIN，由用户程序自己决定何时重试。
 *
 * 八、并发和正确性
 *
 *   .poll 回调可能在数据刚被其他任务取走后才执行，因此返回的就绪
 *   状态只是一个瞬时观察结果。用户态 read() 仍然要处理 EAGAIN。
 *   驱动不能假设“poll 返回可读后，数据永远存在”。
 *
 *   poll_wait() 可以在进程上下文中注册等待项；wake_up_*() 可以从合适
 *   的中断或进程上下文调用。wait_event 和 mutex 的睡眠限制仍然适用：
 *   不能在硬中断、软中断或持有 raw_spinlock_t 时睡眠。
 *
 * ==================== poll API 速查结束 ====================
 */

static int si_poll_open(struct inode *inode, struct file *file)
{
	pr_info("si_poll08: device opened\n");
	return 0;
}

static int si_poll_release(struct inode *inode, struct file *file)
{
	pr_info("si_poll08: device closed\n");
	return 0;
}

static ssize_t si_poll_read(struct file *file, char __user *user_buffer,
				    size_t count, loff_t *offset)
{
	ssize_t ret;

	if (count == 0)
		return 0;

	for (;;) {
		if (mutex_lock_interruptible(&buffer_lock))
			return -ERESTARTSYS;

		if (data_ready)
			break;

		mutex_unlock(&buffer_lock);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* 阻塞式 read：没有数据时等待 write() 修改 data_ready 并唤醒。 */
		ret = wait_event_interruptible(read_queue,
					       READ_ONCE(data_ready));
		if (ret)
			return -ERESTARTSYS;
	}

	if (count < device_size) {
		/* 缓冲区太小，保留数据，用户可以扩大 buffer 后重试。 */
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
	data_ready = false;

unlock:
	mutex_unlock(&buffer_lock);
	return ret;
}

static ssize_t si_poll_write(struct file *file,
				     const char __user *user_buffer,
				     size_t count, loff_t *offset)
{
	if (count == 0)
		return 0;

	if (count >= BUFFER_SIZE)
		count = BUFFER_SIZE - 1;

	if (mutex_lock_interruptible(&buffer_lock))
		return -ERESTARTSYS;

	if (copy_from_user(device_buffer, user_buffer, count)) {
		mutex_unlock(&buffer_lock);
		return -EFAULT;
	}

	device_size = count;
	data_ready = true;
	*offset = 0;
	mutex_unlock(&buffer_lock);

	pr_info("si_poll08: write stored %zu bytes, waking poll/read\n", count);
	wake_up_interruptible(&read_queue);
	return count;
}

static __poll_t si_poll_poll(struct file *file,
				     struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	/* 注册等待队列；poll_wait() 不会在这里睡眠。 */
	poll_wait(file, &read_queue, wait);

	if (READ_ONCE(data_ready))
		mask |= POLLIN | POLLRDNORM;

	/* 本课的缓冲区允许覆盖写入，所以始终报告可写。 */
	mask |= POLLOUT | POLLWRNORM;

	pr_info("si_poll08: .poll returns mask=0x%lx\n",
		(unsigned long)mask);
	return mask;
}

static const struct file_operations si_poll_fops = {
	.owner = THIS_MODULE,
	.open = si_poll_open,
	.read = si_poll_read,
	.write = si_poll_write,
	.poll = si_poll_poll,
	.release = si_poll_release,
};

static int __init si_poll_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&device_number, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&si_cdev, &si_poll_fops);
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

	pr_info("si_poll08: registered /dev/%s major=%d minor=%d\n",
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

static void __exit si_poll_exit(void)
{
	device_destroy(si_class, device_number);
	class_destroy(si_class);
	cdev_del(&si_cdev);
	unregister_chrdev_region(device_number, 1);
	pr_info("si_poll08: unregistered\n");
}

module_init(si_poll_init);
module_exit(si_poll_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 08: poll and non-blocking character device");
