/*
 * Lesson 17：内核线程 kthread
 *
 * 这一课回答一个问题：内核里的“线程”到底是什么？
 *
 * 用户程序通过 clone()/pthread_create() 创建用户线程；内核模块则可以
 * 使用 kthread_run() 创建一个只在内核空间执行的任务。它也有自己的
 * task_struct、PID、调度状态和 current，只是没有用户空间代码和用户态
 * 地址空间。
 *
 * 本模块创建一个名为 si_kthread17 的内核线程。线程每隔 period_ms 毫秒
 * 打印一次自己的信息，然后继续等待。卸载模块时使用 kthread_stop()：
 *
 *   1. 设置线程的 should_stop 标志；
 *   2. 唤醒线程，让它有机会离开等待；
 *   3. 等待线程函数真正返回。
 *
 * 因此，模块退出前必须先停止自己创建的线程。不能只释放模块内存，
 * 否则线程下一次运行时会跳到已经被卸载的代码，造成崩溃。
 *
 * 线程循环中的 wait_event_interruptible_timeout() 用来休眠。休眠期间
 * 它不会占用 CPU；超时或被 kthread_stop() 唤醒后才会继续执行。这里的
 * wait queue 只是线程的休眠/唤醒入口，Lesson 7 已经学习过 wait queue
 * 的基本机制。
 *
 * 运行时可观察：
 *
 *   cat /proc/<pid>/comm
 *   ps -T
 *   dmesg | tail
 *
 * 重点观察日志中的 current->comm、current->pid 和 CPU：它们说明内核
 * 线程也是可被调度的 task，而不是“加载模块时只执行一次的函数”。
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/wait.h>

#define MODULE_NAME "si_kthread17"

static unsigned int period_ms = 1000;
module_param(period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "Thread print period in milliseconds (minimum 1)");

static struct task_struct *worker;
static wait_queue_head_t worker_wq;

static int si_kthread17_fn(void *unused)
{
	unsigned int count = 0;

	/* 内核线程函数从这里开始运行，直到 return 或 kthread_should_stop。 */
	while (!kthread_should_stop()) {
		pr_info(MODULE_NAME ": thread running: count=%u current=%s pid=%d cpu=%u\n",
			count++, current->comm, current->pid, raw_smp_processor_id());

		/*
		 * 条件一旦为真，wait_event 会立即返回；否则线程进入睡眠。
		 * kthread_stop() 会令 kthread_should_stop() 为真并唤醒线程，
		 * 所以卸载时不必等完整的 period_ms 才能退出。
		 */
		wait_event_interruptible_timeout(worker_wq,
						 kthread_should_stop(),
						 msecs_to_jiffies(period_ms));
	}

	pr_info(MODULE_NAME ": thread stopping: current=%s pid=%d\n",
		current->comm, current->pid);
	return 0;
}

static int __init si_kthread17_init(void)
{
	if (period_ms == 0)
		period_ms = 1;

	init_waitqueue_head(&worker_wq);

	/* kthread_run() = 创建线程 + 唤醒线程。失败时返回 ERR_PTR。 */
	worker = kthread_run(si_kthread17_fn, NULL, MODULE_NAME);
	if (IS_ERR(worker)) {
		int ret = PTR_ERR(worker);

		worker = NULL;
		pr_err(MODULE_NAME ": kthread_run failed: %d\n", ret);
		return ret;
	}

	pr_info(MODULE_NAME ": loaded, pid=%d period_ms=%u\n",
		worker->pid, period_ms);
	return 0;
}

static void __exit si_kthread17_exit(void)
{
	if (!worker)
		return;

	pr_info(MODULE_NAME ": stopping thread pid=%d\n", worker->pid);
	/* kthread_stop() 返回线程函数的返回值，并等待它真正结束。 */
	kthread_stop(worker);
	worker = NULL;
	pr_info(MODULE_NAME ": unloaded\n");
}

module_init(si_kthread17_init);
module_exit(si_kthread17_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI kernel learning");
MODULE_DESCRIPTION("Lesson 17: kernel thread and safe stop");
