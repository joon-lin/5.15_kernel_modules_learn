#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/sched.h>

#define DRIVER_NAME "si_irq11"

/*
 * 必须由加载模块的人指定真实、可用的硬件 IRQ：
 *
 *     insmod si_irq11.ko irq_number=<IRQ>
 *
 * 不要随便从 /proc/interrupts 选一个正在使用的 IRQ。这个练习的顶半部
 * 会对每次该 IRQ 都返回 IRQ_WAKE_THREAD，因此没有真实设备状态检查，
 * 不适合直接挂到共享 IRQ。后续 platform driver + GPIO/设备树课程会用
 * platform_get_irq() 得到正确 IRQ，并在顶半部确认中断确实来自本设备。
 */
static int irq_number = -1;
module_param(irq_number, int, 0444);
MODULE_PARM_DESC(irq_number, "Hardware IRQ number used by this lesson");

static void print_irq_context(const char *where)
{
	pr_info("si_irq11: %s current=%s pid=%d in_interrupt=%d "
		"in_hardirq=%d in_softirq=%d irqs_disabled=%d\n",
		where, current->comm, (int)current->pid,
		(int)in_interrupt(), (int)in_hardirq(), (int)in_softirq(),
		(int)irqs_disabled());
}

/*
 * ==================== IRQ API 速查 ====================
 *
 * 一、中断处理的基本流程
 *
 *   硬件产生 IRQ
 *       ↓
 *   内核进入 primary handler（顶半部）
 *       ↓
 *   快速确认/清除设备中断状态
 *       ↓
 *   需要复杂处理时返回 IRQ_WAKE_THREAD
 *       ↓
 *   threaded handler（线程函数）被唤醒
 *       ↓
 *   在线程上下文中完成耗时或可睡眠的工作
 *
 * 顶半部运行时必须按原子/硬中断上下文编写：不能 msleep()、schedule()、
 * mutex_lock() 或 kmalloc(..., GFP_KERNEL)。线程函数属于进程上下文，
 * 通常可以睡眠和使用 mutex。
 *
 * 二、申请 IRQ
 *
 *   request_irq(irq, handler, flags, name, dev_id);
 *       注册一个只有顶半部的 IRQ handler：
 *
 *       static irqreturn_t handler(int irq, void *dev_id)
 *       {
 *           return IRQ_HANDLED;
 *       }
 *
 *   request_threaded_irq(irq, handler, thread_fn, flags, name, dev_id);
 *       注册线程化 IRQ。本课使用：
 *
 *       request_threaded_irq(irq_number,
 *                            si_irq_top,
 *                            si_irq_thread,
 *                            IRQF_ONESHOT,
 *                            DRIVER_NAME,
 *                            &irq_number);
 *
 *       handler 是顶半部；thread_fn 是线程函数。handler 确认中断属于本
 *       设备后返回 IRQ_WAKE_THREAD，内核才会唤醒 thread_fn。
 *
 *   request_any_context_irq(...);
 *       在某些内核路径中请求一个可能运行在硬中断或嵌套线程上下文的
 *       handler。普通设备驱动优先明确使用 request_irq 或
 *       request_threaded_irq。
 *
 * 三、handler 的返回值
 *
 *   IRQ_NONE
 *       中断不是本设备产生的。共享 IRQ 时必须认真判断并返回它。
 *
 *   IRQ_HANDLED
 *       本设备已经处理了中断。
 *
 *   IRQ_WAKE_THREAD
 *       顶半部已经完成必要的快速处理，请求唤醒 thread_fn。通常使用
 *       IRQ_WAKE_THREAD，而不是单独返回 IRQ_HANDLED | IRQ_WAKE_THREAD。
 *
 * 四、常见 IRQ flags
 *
 *   IRQF_ONESHOT
 *       线程函数执行期间保持 IRQ 屏蔽，避免同一中断反复进入。线程化
 *       IRQ 通常需要它；本课使用它。
 *
 *   IRQF_SHARED
 *       允许多个设备共享一条 IRQ。此时 dev_id 必须非 NULL，每个顶半部
 *       都必须检查中断是否来自自己；卸载时用同一个 dev_id 调 free_irq。
 *
 *   IRQF_TRIGGER_RISING
 *   IRQF_TRIGGER_FALLING
 *   IRQF_TRIGGER_HIGH
 *   IRQF_TRIGGER_LOW
 *       指定边沿或电平触发。设备树/固件已经正确配置时，驱动通常不要
 *       擅自覆盖触发类型。
 *
 *   IRQF_NO_AUTOEN
 *       注册后不自动打开 IRQ，需要之后显式 enable_irq()。
 *
 *   IRQF_NO_SUSPEND
 *       系统挂起时不自动关闭，只有确实支持该行为的唤醒设备才使用。
 *
 *   IRQF_NO_THREAD
 *       该 IRQ 不能被线程化，只有特殊的低级 IRQ 才使用。
 *
 * 五、释放和控制 IRQ
 *
 *   free_irq(irq, dev_id);
 *       删除自己的 handler，并等待正在运行的处理函数完成。模块卸载
 *       前必须调用，否则中断可能跳转到已经卸载的模块代码。
 *
 *   synchronize_irq(irq);
 *       等待该 IRQ 上已经开始的处理完成，但不负责删除 handler。
 *
 *   disable_irq(irq);
 *       禁止 IRQ，并等待当前正在执行的 handler 完成。可能睡眠，不能在
 *       硬中断上下文调用。
 *
 *   disable_irq_nosync(irq);
 *       禁止 IRQ，但不等待正在执行的 handler。可以用于原子上下文，但
 *       后续必须正确同步，不能直接释放相关资源。
 *
 *   enable_irq(irq);
 *       重新打开此前 disable_irq() 或 disable_irq_nosync() 禁止的 IRQ。
 *       disable/enable 必须正确配对。
 *
 * 六、设备管理版本
 *
 *   devm_request_irq()
 *   devm_request_threaded_irq()
 *   devm_free_irq()
 *
 *   这些是 devres 资源管理版本，适合 platform_driver 的 probe/remove。
 *   设备释放时 devres 会自动回收，减少错误路径清理代码；本课是独立
 *   模块，所以使用手动 request/free 版本。
 *
 * 七、dev_id 为什么重要
 *
 *   dev_id 是传给 handler 的私有指针，也是识别共享 IRQ 和释放 handler
 *   的凭据。通常传入设备结构体地址：
 *
 *       request_irq(irq, handler, IRQF_SHARED, "mydev", &mydev);
 *       free_irq(irq, &mydev);
 *
 *   request/free 两处必须使用同一个指针，不能传两个内容相同但地址不同
 *   的对象。
 *
 * 八、当前 PREEMPT_RT 的注意事项
 *
 *   当前 GJ3 内核 CONFIG_PREEMPT_RT=y。IRQ 线程化会让更多中断处理在线程
 *   中运行，但 primary handler 仍应按最严格的硬中断规则编写；只有
 *   thread_fn 才明确放置可睡眠工作。这样代码在 RT 和非 RT 内核上都安全。
 *
 * ==================== IRQ API 速查结束 ====================
 */

/* 顶半部：快速返回，不能睡眠。 */
static irqreturn_t si_irq_top(int irq, void *dev_id)
{
	(void)dev_id;
	pr_info("si_irq11: top half received irq=%d\n", irq);

	/* 真实驱动应先确认并清除硬件中断状态，再唤醒线程函数。 */
	return IRQ_WAKE_THREAD;
}

/* 线程函数：可以睡眠，执行复杂处理。 */
static irqreturn_t si_irq_thread(int irq, void *dev_id)
{
	(void)dev_id;
	print_irq_context("threaded handler before sleep");

	/* 教学演示：线程化 handler 可以睡眠。 */
	msleep(20);

	print_irq_context("threaded handler after sleep");
	return IRQ_HANDLED;
}

static int __init si_irq_init(void)
{
	int ret;

	if (irq_number < 0) {
		pr_err("si_irq11: specify a real IRQ with irq_number=<IRQ>\n");
		return -EINVAL;
	}

	ret = request_threaded_irq(irq_number,
				   si_irq_top,
				   si_irq_thread,
				   IRQF_ONESHOT,
				   DRIVER_NAME,
				   &irq_number);
	if (ret) {
		pr_err("si_irq11: request_threaded_irq(%d) failed: %d\n",
		       irq_number, ret);
		return ret;
	}

	pr_info("si_irq11: registered threaded IRQ %d\n", irq_number);
	return 0;
}

static void __exit si_irq_exit(void)
{
	free_irq(irq_number, &irq_number);
	pr_info("si_irq11: freed IRQ %d\n", irq_number);
}

module_init(si_irq_init);
module_exit(si_irq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 11: hard IRQ and threaded IRQ");

