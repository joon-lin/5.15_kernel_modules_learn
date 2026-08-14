#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/sched.h>

#define DRIVER_NAME "si_irq11_plain"

/* 使用一个真实、可申请且不会影响系统的 IRQ 进行实验。 */
static int irq_number = -1;
module_param(irq_number, int, 0444);
MODULE_PARM_DESC(irq_number, "Hardware IRQ number used by this lesson");

/*
 * ==================== request_irq 对比实验 ====================
 *
 * request_irq() 是只有一个 handler 的中断注册接口：
 *
 *   request_irq(irq, handler, flags, name, dev_id);
 *
 * 它等价于：
 *
 *   request_threaded_irq(irq, handler, NULL, flags, name, dev_id);
 *
 * 因为没有 thread_fn，所以这个模块只注册一个普通 handler。
 *
 * 非 RT 内核通常直接在硬中断上下文执行 handler：
 *
 *   硬件 IRQ -> handler()
 *
 * PREEMPT_RT 或使用 threadirqs 启动参数时，内核可能强制把普通 IRQ
 * 线程化：
 *
 *   硬件 IRQ -> 很短的顶半部 -> irq/<n>-si_irq11_plain 线程 -> handler()
 *
 * 因此同一个 request_irq() 程序，在 RT 和非 RT 上的 handler 上下文
 * 可能不同。请看 dmesg 中打印的 in_interrupt()、in_hardirq() 和
 * current->comm。
 *
 * handler 的通用规则仍然必须按最严格情况编写：
 *
 *   不能 msleep()、schedule()、wait_event()；
 *   不能 mutex_lock()；
 *   不能使用 kmalloc(..., GFP_KERNEL)；
 *   只做快速确认、清除设备状态和记录事件。
 *
 * 返回值：
 *
 *   IRQ_NONE      不是本设备的中断，尤其用于共享 IRQ；
 *   IRQ_HANDLED   本设备已经处理了中断。
 *
 * free_irq(irq, dev_id) 会删除 handler，并等待已经开始的处理结束。
 * 模块卸载前必须使用和 request_irq() 完全相同的 dev_id。
 *
 * 注意：本实验的 handler 会无条件返回 IRQ_HANDLED，因此不能使用
 * IRQF_SHARED，也不能随便挂到系统正在使用的 IRQ 上。不要和
 * si_irq11.ko 同时申请同一个 IRQ。
 *
 * ==================== request_irq 对比实验结束 ====================
 */

static irqreturn_t si_plain_handler(int irq, void *dev_id)
{
	(void)dev_id;

	pr_info("si_irq11_plain: handler irq=%d current=%s pid=%d "
		"in_interrupt=%d in_hardirq=%d in_softirq=%d "
		"irqs_disabled=%d\n",
		irq, current->comm, (int)current->pid,
		(int)in_interrupt(), (int)in_hardirq(), (int)in_softirq(),
		(int)irqs_disabled());

	return IRQ_HANDLED;
}

static int __init si_plain_init(void)
{
	int ret;

	if (irq_number < 0) {
		pr_err("si_irq11_plain: specify irq_number=<IRQ>\n");
		return -EINVAL;
	}

	ret = request_irq(irq_number, si_plain_handler, 0,
				 DRIVER_NAME, &irq_number);
	if (ret) {
		pr_err("si_irq11_plain: request_irq(%d) failed: %d\n",
		       irq_number, ret);
		return ret;
	}

	pr_info("si_irq11_plain: registered IRQ %d\n", irq_number);
	return 0;
}

static void __exit si_plain_exit(void)
{
	free_irq(irq_number, &irq_number);
	pr_info("si_irq11_plain: freed IRQ %d\n", irq_number);
}

module_init(si_plain_init);
module_exit(si_plain_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 11: request_irq RT versus non-RT comparison");
