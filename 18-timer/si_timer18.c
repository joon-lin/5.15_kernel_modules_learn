/*
 * Lesson 18：PREEMPT_RT 下的硬中断定时器 hrtimer
 *
 * 内核定时器的意思是：“现在登记一个时间点，时间到了以后调用我的
 * 回调函数”。设置定时器的函数不会在这里等待，模块初始化可以立即
 * 返回；到期后由内核定时器机制异步调用 si_timer18_callback()。
 *
 * 本课使用高精度定时器 hrtimer，并明确指定 HRTIMER_MODE_REL_PINNED_HARD：
 * 即使当前内核启用了 PREEMPT_RT，回调仍然在硬中断上下文执行，并且
 * 固定在启动定时器时所在的 CPU。加载模块后每隔 period_ms 毫秒执行一次
 * 回调。这能清楚地观察定时器和
 * 内核线程的区别：
 *
 *   kthread：有一个独立的 task/PID，可以自己循环和睡眠；
 *   hrtimer：没有独立线程，每次到期时直接执行一次回调。
 *
 * hrtimer 使用 ktime_t 表示高精度时间。本课使用 ms_to_ktime() 把毫秒
 * 转换成 ktime_t，而不是把毫秒直接传给 hrtimer_start()。
 *
 * HRTIMER_MODE_REL_PINNED_HARD 的回调必须短小，不能睡眠：不能调用 msleep()、
 * mutex_lock()（可能阻塞）或 wait_event()。如果到期后需要执行较重的
 * 工作，应在回调中 schedule_work()，把工作交给 workqueue。
 *
 * 回调返回 HRTIMER_RESTART 并调用 hrtimer_forward_now()，把下一次到期
 * 时间向前推进一个周期。卸载模块时必须先 hrtimer_cancel()，它会删除
 * 尚未到期的定时器，并等待正在执行的回调结束，确保回调不再访问即将
 * 从内核中卸载的模块代码。
 *
 * 本课还测量“定时器预定到期时间”到“回调真正开始执行”之间的延迟：
 *
 *   late_ns = ktime_get_mono_fast_ns() - hrtimer_get_expires(timer)
 *
 * late_ns 越大，说明这次硬中断响应越晚。这里测的是定时器回调的
 * 软件延迟，不是外部 GPIO 硬件信号到 CPU 的完整 IRQ 延迟。回调中不
 * 每次打印日志，避免 printk 本身干扰测量；默认每 100 次报告一次。
 */

#include <linux/init.h>
#include <linux/hardirq.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/timekeeping.h>

#define MODULE_NAME "si_timer18"

static unsigned int period_ms = 1000;
module_param(period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "Periodic timer interval in milliseconds");

static unsigned int report_every = 100;
module_param(report_every, uint, 0644);
MODULE_PARM_DESC(report_every, "Print one latency sample every N callbacks");

static struct hrtimer lesson_timer;
static unsigned int fired_count;
static u64 total_late_ns;
static u64 min_late_ns = ~0ULL;
static u64 max_late_ns;

static enum hrtimer_restart si_timer18_callback(struct hrtimer *timer)
{
	u64 now_ns;
	u64 expected_ns;
	u64 late_ns;
	unsigned int period;
	unsigned int sample;

	/* 在推进下一次到期时间前，读取本次定时器原本的 deadline。 */
	now_ns = ktime_get_mono_fast_ns();
	expected_ns = (u64)ktime_to_ns(hrtimer_get_expires(timer));
	late_ns = now_ns > expected_ns ? now_ns - expected_ns : 0;

	sample = ++fired_count;
	total_late_ns += late_ns;
	if (late_ns < min_late_ns)
		min_late_ns = late_ns;
	if (late_ns > max_late_ns)
		max_late_ns = late_ns;

	/* 只偶尔打印，避免 printk 干扰后续的延迟测量。 */
	if (sample == 1 ||
	    (READ_ONCE(report_every) && sample % READ_ONCE(report_every) == 0))
		pr_info(MODULE_NAME ": sample=%u late_ns=%llu current=%s pid=%d "
			"cpu=%u in_hardirq=%d\n",
			sample, (unsigned long long)late_ns, current->comm,
			current->pid, raw_smp_processor_id(), !!in_hardirq());

	/*
	 * 以当前到期时间为基准推进下一次到期时间，避免因为回调耗时
	 * 产生越来越大的漂移。HRTIMER_RESTART 表示继续运行定时器。
	 */
	period = READ_ONCE(period_ms);
	if (period == 0)
		period = 1;
	hrtimer_forward_now(timer, ms_to_ktime(period));
	return HRTIMER_RESTART;
}

static int __init si_timer18_init(void)
{
	if (period_ms == 0)
		period_ms = 1;

	/*
	 * RT 内核需要在初始化时就标记为 HARD；启动时的模式必须与这里
	 * 一致，否则 hrtimer 会走 softirq 定时器队列。PINNED 表示固定在
	 * hrtimer_start() 执行时所在的 CPU。
	 */
	hrtimer_init(&lesson_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL_PINNED_HARD);
	lesson_timer.function = si_timer18_callback;

	pr_info(MODULE_NAME ": scheduling periodic timer: period_ms=%u "
		"mode=HRTIMER_MODE_REL_PINNED_HARD start_cpu=%u\n",
		period_ms, raw_smp_processor_id());

	/*
	 * hrtimer_start() 会启动定时器；时间到了才执行 callback。
	 * REL_PINNED_HARD 是关键：在 PREEMPT_RT 下请求 hard IRQ 上下文，
	 * 同时固定到当前 CPU。
	 */
	hrtimer_start(&lesson_timer, ms_to_ktime(period_ms),
		      HRTIMER_MODE_REL_PINNED_HARD);

	return 0;
}

static void __exit si_timer18_exit(void)
{
	u64 average_late_ns;
	/* 必须在模块卸载前取消定时器，并等待可能正在运行的回调。 */
	hrtimer_cancel(&lesson_timer);
	if (fired_count) {
		average_late_ns = total_late_ns;
		/* do_div() 使用内核的 32 位除法实现，避免 ARM libc 辅助符号。 */
		do_div(average_late_ns, fired_count);
		pr_info(MODULE_NAME ": samples=%u late_ns(min/avg/max)=%llu/%llu/%llu "
			"jitter_range_ns=%llu\n", fired_count,
			(unsigned long long)min_late_ns,
			(unsigned long long)average_late_ns,
			(unsigned long long)max_late_ns,
			(unsigned long long)(max_late_ns - min_late_ns));
	} else
		pr_info(MODULE_NAME ": no samples\n");
}

module_init(si_timer18_init);
module_exit(si_timer18_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI kernel learning");
MODULE_DESCRIPTION("Lesson 18: pinned hard-IRQ hrtimer on PREEMPT_RT");
