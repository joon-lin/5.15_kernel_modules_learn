#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

/*
 * ==================== workqueue API 速查 ====================
 *
 * workqueue 是“把工作延后到内核工作线程中执行”的机制。
 * 常见使用场景是：中断处理函数只做很少的事情，把可能睡眠、耗时或
 * 需要调用复杂内核 API 的部分交给 workqueue。
 *
 * 中断上下文：
 *
 *     不能睡眠，不能调用 mutex_lock()、msleep()、GFP_KERNEL 等。
 *
 * workqueue 回调：
 *
 *     在内核工作线程中运行，属于进程上下文，通常可以睡眠。
 *     但仍然要正确使用 mutex 等同步手段保护共享数据。
 *
 * 一、工作项类型和初始化
 *
 *   struct work_struct work;
 *   INIT_WORK(&work, work_function);
 *
 *   static void work_function(struct work_struct *work)
 *   {
 *       // 工作线程中执行
 *   }
 *
 *   struct delayed_work delayed;
 *   INIT_DELAYED_WORK(&delayed, delayed_function);
 *
 *   delayed_work 由一个 timer 和一个 work_struct 组成：先等待延时，
 *   再把 work 放到 workqueue 中执行。
 *
 * 二、选择 workqueue
 *
 *   queue_work(system_wq, &work);
 *       把工作放入内核全局 system_wq。小型、简单的工作常用它。
 *
 *   schedule_work(&work);
 *       queue_work(system_wq, &work) 的简写。
 *
 *   alloc_workqueue("name", flags, max_active);
 *       创建驱动自己的 workqueue。本课使用自己的队列：
 *
 *       workqueue = alloc_workqueue("si_lesson09",
 *                                   WQ_UNBOUND | WQ_MEM_RECLAIM,
 *                                   1);
 *
 *       max_active=1 表示同一时间最多执行一个工作项，便于观察顺序。
 *       WQ_UNBOUND 表示不绑定提交工作的 CPU；WQ_MEM_RECLAIM 适合内存
 *       回收相关路径，避免内存压力下工作无法运行。
 *
 *   alloc_ordered_workqueue("name", flags);
 *       创建按提交顺序、同一时间只执行一个工作的队列。需要严格顺序
 *       时可以使用它。
 *
 * 三、提交工作
 *
 *   queue_work(workqueue, &work);
 *       把普通 work 放入指定队列。返回值：
 *
 *       true    这次成功加入队列
 *       false   work 已经在队列中或正在执行，未重复加入
 *
 *       同一个 work_struct 在执行结束前不能重复排队；若需要再次执行，
 *       在回调结束后或确认它不再 pending 后重新 queue_work()。
 *
 *   queue_delayed_work(workqueue, &delayed, delay);
 *       delay 单位是 jiffies，延迟结束后再执行 delayed work：
 *
 *       queue_delayed_work(workqueue, &delayed,
 *                          msecs_to_jiffies(1000));
 *
 *   schedule_delayed_work(&delayed, delay);
 *       把 delayed work 放入全局 system_wq 的简写。
 *
 *   mod_delayed_work(workqueue, &delayed, delay);
 *       如果 delayed work 已经排队，就修改/重新开始延时；如果尚未排队，
 *       则把它加入队列。周期性任务常用它实现“从这次开始再等 N ms”。
 *
 *   queue_work_on(cpu, workqueue, &work);
 *   queue_delayed_work_on(cpu, workqueue, &delayed, delay);
 *       请求在指定 CPU 上执行。除非确实需要 CPU 亲和性，否则优先使用
 *       不带 _on 的版本。
 *
 * 四、等待、取消和清理
 *
 *   flush_work(&work);
 *       等待指定 work 已经完成；不会取消它。
 *
 *   flush_delayed_work(&delayed);
 *       等待 delayed work 完成；如果仍在等待延时，通常会把它推进执行。
 *
 *   flush_workqueue(workqueue);
 *       等待指定 workqueue 中已经排队的工作完成。范围较大，通常不如
 *       针对单个 work 的 flush/cancel 精确。
 *
 *   cancel_work_sync(&work);
 *       取消尚未执行的 work；如果它已经开始执行，则等待它执行完毕。
 *       _sync 很重要：返回后可以确定回调不再运行。
 *
 *   cancel_delayed_work(&delayed);
 *       尝试取消延时计时器，但不等待已经开始执行的回调。
 *
 *   cancel_delayed_work_sync(&delayed);
 *       取消 delayed work，并等待可能正在执行的回调结束。模块卸载时
 *       通常应使用这个版本，避免回调在模块代码已卸载后继续运行。
 *
 *   destroy_workqueue(workqueue);
 *       销毁驱动自己创建的 workqueue。销毁前应先取消/同步所有本模块
 *       的 work，尤其是 delayed_work。
 *
 * 五、延时单位
 *
 *   workqueue 的 delay 参数使用 jiffies，而不是毫秒：
 *
 *       msecs_to_jiffies(500);  // 500 ms
 *       secs_to_jiffies(2);     // 2 s，若内核提供该宏
 *       jiffies_to_msecs(j);    // jiffies 转毫秒
 *
 *   不要直接把 500 当成“500 ms”传入，因为 HZ 不一定等于 1000。
 *
 * 六、work 回调中的注意事项
 *
 *   1. work 回调属于进程上下文，通常可以 msleep()、mutex_lock()，
 *      也可以使用 GFP_KERNEL 分配内存；但不能无限期阻塞。
 *
 *   2. work 回调可能和 read/write、定时器或另一个 work 并发运行，
 *      共享变量仍然需要 mutex、spinlock 或原子操作保护。
 *
 *   3. 不要把栈上 work 交给异步队列后立刻返回；栈帧消失后 work 指针
 *      就失效。异步 work 应放在静态变量、长期存活的结构体或动态内存中。
 *
 *   4. 回调中不要直接使用已经释放的设备对象。卸载或释放对象前，必须
 *      cancel_*_sync()，确保回调已经退出。
 *
 * 七、和 timer、tasklet、中断下半部的区别
 *
 *   timer 回调：通常仍属于软中断上下文，不能睡眠。
 *   tasklet：软中断上下文，不能睡眠。
 *   workqueue：内核线程进程上下文，通常可以睡眠。
 *
 *   口诀：
 *
 *       timer/tasklet 不能睡眠；workqueue 可以睡眠。
 *
 * ==================== workqueue API 速查结束 ====================
 */

static struct workqueue_struct *lesson_wq;
static struct work_struct immediate_work;
static struct delayed_work delayed_work_item;

static unsigned int delay_ms = 2000;
module_param(delay_ms, uint, 0644);
MODULE_PARM_DESC(delay_ms, "Delay before delayed work runs, in milliseconds");

static void immediate_work_handler(struct work_struct *work)
{
	pr_info("si_workqueue09: immediate work starts, current=%s pid=%d\n",
		current->comm, current->pid);

	/* 这里故意睡眠，证明 workqueue 回调运行在可睡眠的进程上下文。 */
	msleep(100);

	pr_info("si_workqueue09: immediate work finished\n");
}

static void delayed_work_handler(struct work_struct *work)
{
	pr_info("si_workqueue09: delayed work starts after %u ms, current=%s pid=%d\n",
		delay_ms, current->comm, current->pid);

	/* delayed_work 的回调参数是内部的 work_struct。 */
	msleep(100);

	pr_info("si_workqueue09: delayed work finished\n");
}

static int __init si_workqueue_init(void)
{
	bool queued;

	if (delay_ms > 60000)
		delay_ms = 60000;

	lesson_wq = alloc_workqueue("si_lesson09",
					   WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!lesson_wq)
		return -ENOMEM;

	INIT_WORK(&immediate_work, immediate_work_handler);
	INIT_DELAYED_WORK(&delayed_work_item, delayed_work_handler);

	queued = queue_work(lesson_wq, &immediate_work);
	pr_info("si_workqueue09: queue immediate work -> %s\n",
		queued ? "queued" : "already pending");

	queued = queue_delayed_work(lesson_wq, &delayed_work_item,
					msecs_to_jiffies(delay_ms));
	pr_info("si_workqueue09: queue delayed work -> %s\n",
		queued ? "queued" : "already pending");

	pr_info("si_workqueue09: loaded, watch dmesg for worker callbacks\n");
	return 0;
}

static void __exit si_workqueue_exit(void)
{
	/* 返回后保证 delayed_work 的回调不再运行。 */
	cancel_delayed_work_sync(&delayed_work_item);

	/* 返回后保证普通 work 的回调不再运行。 */
	cancel_work_sync(&immediate_work);

	destroy_workqueue(lesson_wq);
	pr_info("si_workqueue09: unloaded\n");
}

module_init(si_workqueue_init);
module_exit(si_workqueue_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 09: workqueue and delayed_work");
