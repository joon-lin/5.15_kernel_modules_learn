#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/sched.h>

/*
 * ==================== softirq / tasklet API 速查 ====================
 *
 * 一、softirq 是什么
 *
 * softirq 是内核预先定义的一组“软中断向量”。每个向量有一个编号和
 * 一个处理函数，常见类型包括：
 *
 *   HI_SOFTIRQ       高优先级 tasklet
 *   TIMER_SOFTIRQ    timer 软中断
 *   NET_TX_SOFTIRQ   网络发送
 *   NET_RX_SOFTIRQ   网络接收
 *   BLOCK_SOFTIRQ    块设备相关处理
 *   TASKLET_SOFTIRQ  普通 tasklet
 *   SCHED_SOFTIRQ    调度域负载均衡
 *   HRTIMER_SOFTIRQ  高精度定时器
 *   RCU_SOFTIRQ      RCU 回调
 *
 * 硬中断通常只做最紧急的事情，然后设置 softirq pending 位；softirq
 * 在稍后执行。softirq 可能在硬中断返回路径执行，也可能由每 CPU 的
 * ksoftirqd/N 内核线程执行。
 *
 *   raise_softirq(nr);
 *       设置一个 softirq pending 位，并请求稍后处理。这个接口是给内核
 *       已注册的 softirq 使用的，普通模块不能凭空创建新的 softirq 类型。
 *
 *   raise_softirq_irqoff(nr);
 *       调用者已经关闭本地中断时使用的版本。
 *
 *   open_softirq(nr, action);
 *       把编号 nr 绑定到 action。它是内核初始化阶段使用的内部接口，
 *       不是普通模块注册自定义 softirq 的通用方法；本模块不调用它。
 *
 *   softirq_init();
 *       初始化内置 softirq 机制，由内核启动流程调用，不由驱动调用。
 *
 * 二、softirq 上下文的规则
 *
 * softirq 回调不是普通进程上下文，必须遵守原子上下文规则：
 *
 *   不能调用 msleep()、schedule()、wait_event()；
 *   不能获取可能睡眠的 mutex；
 *   不能使用 GFP_KERNEL 分配内存；
 *   不能执行很长时间的循环。
 *
 * 需要睡眠或复杂处理时，softirq/tasklet 只负责快速记录事件，然后
 * queue_work() 交给 workqueue。口诀：
 *
 *   softirq/tasklet 不能睡眠；workqueue 可以睡眠。
 *
 * 三、tasklet 是什么
 *
 * tasklet 是建立在 TASKLET_SOFTIRQ 之上的更简单接口。它保证同一个
 * tasklet 不会同时在两个 CPU 上运行，但不同 tasklet 可以在不同 CPU
 * 并行执行。tasklet API 已被内核标记为 deprecated，新驱动优先使用
 * threaded IRQ 或 workqueue；这里保留它是为了学习历史驱动模型。
 *
 *   struct tasklet_struct tasklet;
 *
 *   tasklet_setup(&tasklet, callback);
 *       初始化新的 callback 风格 tasklet。回调签名是：
 *
 *       static void callback(struct tasklet_struct *t);
 *
 *   tasklet_init(&tasklet, old_callback, data);
 *       初始化旧的 unsigned long data 风格 tasklet。新代码优先使用
 *       tasklet_setup()，旧接口只为阅读老驱动时认识。
 *
 *   DECLARE_TASKLET(name, callback);
 *   DECLARE_TASKLET_DISABLED(name, callback);
 *       静态声明并初始化 tasklet。callback 版本与 tasklet_setup() 一致。
 *
 *   tasklet_schedule(&tasklet);
 *       把普通 tasklet 加入当前 CPU 的 TASKLET_SOFTIRQ 队列。
 *       如果它已经 pending，不会重复排队；回调至少会执行一次。
 *
 *   tasklet_hi_schedule(&tasklet);
 *       加入 HI_SOFTIRQ 队列，优先于普通 tasklet。它并不等于实时线程
 *       优先级，仍然属于 softirq，仍然不能睡眠。
 *
 *   tasklet_disable(&tasklet);
 *       禁止 tasklet 执行，并等待当前已经运行的 tasklet 结束。通常只
 *       在进程上下文使用。
 *
 *   tasklet_disable_nosync(&tasklet);
 *       只增加禁止计数，不等待已经在其他 CPU 上运行的回调结束。
 *       使用后必须匹配 tasklet_enable()。
 *
 *   tasklet_disable_in_atomic(&tasklet);
 *       面向原子上下文的特殊版本；新代码一般应重新设计，避免在原子
 *       上下文里管理 tasklet。
 *
 *   tasklet_enable(&tasklet);
 *       减少禁止计数，重新允许 tasklet 运行。
 *
 *   tasklet_trylock(&tasklet);
 *   tasklet_unlock(&tasklet);
 *   tasklet_unlock_wait(&tasklet);
 *   tasklet_unlock_spin_wait(&tasklet);
 *       tasklet 内部的并发控制接口，普通驱动通常不直接调用。
 *
 *   tasklet_kill(&tasklet);
 *       等待 tasklet 不再 pending、也不再执行。模块卸载前必须调用，
 *       否则 tasklet 可能在模块代码已经卸载后跳到失效地址。
 *       tasklet_kill() 可能等待，不能在中断/softirq 上下文调用。
 *
 * 四、tasklet、softirq、workqueue 的关系
 *
 *   tasklet_schedule()
 *          ↓
 *   TASKLET_SOFTIRQ pending
 *          ↓
 *   tasklet_action()
 *          ↓
 *   tasklet callback
 *
 *   queue_work()
 *          ↓
 *   workqueue
 *          ↓
 *   kworker 线程执行 work callback
 *
 * 因此 tasklet 不是线程，softirq 也不是线程；只有 softirq 被 ksoftirqd
 * 延后处理时，才会看到 ksoftirqd/N 参与执行。当前 GJ3 使用 PREEMPT_RT，
 * softirq 处理更倾向通过线程化的 ksoftirqd 运行，但代码仍应遵守不能在
 * tasklet/softirq 中睡眠的可移植规则；需要睡眠就明确使用 workqueue。
 *
 * 五、如何观察
 *
 *   cat /proc/softirqs
 *       查看每个 CPU 的 softirq 计数，重点观察 TASKLET、TIMER、NET_RX。
 *
 *   ps -e -o pid,cls,rtprio,pri,ni,comm,args | grep -E 'ksoftirqd|kworker'
 *       查看 softirqd 和 workqueue worker 线程。
 *
 *   本课回调中会打印 current->comm、in_softirq()、in_interrupt() 和
 *   irqs_disabled()，用于把“上下文”概念和实际日志对应起来。
 *
 * ==================== softirq / tasklet 速查结束 ====================
 */

static struct tasklet_struct normal_tasklet;
static struct tasklet_struct high_tasklet;

static void print_tasklet_context(const char *name)
{
	pr_info("si_tasklet10: %s current=%s pid=%d cpu=%u "
		"in_softirq=%d in_interrupt=%d irqs_disabled=%d "
		"preempt_count=0x%lx\n",
		name, current->comm, (int)current->pid,
		(unsigned int)smp_processor_id(), (int)in_softirq(),
		(int)in_interrupt(), (int)irqs_disabled(),
		(unsigned long)preempt_count());
}

static void normal_tasklet_callback(struct tasklet_struct *tasklet)
{
	(void)tasklet;
	print_tasklet_context("normal tasklet callback");
}

static void high_tasklet_callback(struct tasklet_struct *tasklet)
{
	(void)tasklet;
	print_tasklet_context("high-priority tasklet callback");
}

static int __init si_tasklet_init(void)
{
	tasklet_setup(&normal_tasklet, normal_tasklet_callback);
	tasklet_setup(&high_tasklet, high_tasklet_callback);

	pr_info("si_tasklet10: scheduling normal and high tasklets\n");
	tasklet_schedule(&normal_tasklet);
	tasklet_hi_schedule(&high_tasklet);

	pr_info("si_tasklet10: loaded, inspect /proc/softirqs and dmesg\n");
	return 0;
}

static void __exit si_tasklet_exit(void)
{
	/* 保证回调完成后，模块代码才会被卸载。 */
	tasklet_kill(&normal_tasklet);
	tasklet_kill(&high_tasklet);
	pr_info("si_tasklet10: unloaded\n");
}

module_init(si_tasklet_init);
module_exit(si_tasklet_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 10: tasklet and softirq context");
