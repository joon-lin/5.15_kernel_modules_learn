/*
 * Lesson 19：RCU（Read-Copy-Update）
 *
 * RCU 适合“读很多、写很少”的共享数据。
 *
 * 本例维护一个由 current_value 指向的配置对象：
 *
 *   读者：
 *     rcu_read_lock()
 *     rcu_dereference(current_value)
 *     复制对象里的字段
 *     rcu_read_unlock()
 *
 *   写者：
 *     分配一个新对象
 *     填好新对象
 *     rcu_assign_pointer() 发布新对象
 *     call_rcu() 延迟释放旧对象
 *
 * 写者不会直接修改正在被读者使用的旧对象，而是先创建新副本。
 * 因此读者不需要像 mutex 那样阻塞其他读者。
 *
 * RCU 的关键点是“宽限期（grace period）”：
 *
 *   旧对象被替换后，已经进入 rcu_read_lock() 的读者仍可以继续使用；
 *   等所有这些读者离开 rcu_read_unlock() 后，call_rcu() 的回调才会执行，
 *   这时释放旧对象才安全。
 *
 * 注意：
 *   1. 指针指向的对象只能在 rcu_read_lock() 和 rcu_read_unlock() 之间使用。
 *      不能把指针保存下来，解锁后再访问，否则可能访问已经释放的对象。
 *   2. 读侧临界区要短，里面不能睡眠；这里只复制字段，不做耗时工作。
 *   3. call_rcu() 的回调不能睡眠。它可能在软中断或 RCU 回调线程中执行。
 *   4. RCU 主要解决“读者之间、读者和写者之间”的并发访问；写者之间仍然
 *      需要自己的互斥机制。本例只有一个 writer_work，因此没有写者竞争。
 *
 * 加载模块后，reader_task 每 500 ms 读取一次，writer_work 每 1000 ms 发布
 * 一个新对象。可以从 dmesg 观察：读者看到的版本、旧对象何时被回收，以及
 * “替换对象”和“释放旧对象”之间为什么不是同一时刻。
 *
 * 本课涉及的 RCU API：
 *
 *   struct rcu_head
 *       放在需要延迟回收的对象里。call_rcu() 会使用它保存回调信息。
 *
 *   __rcu
 *       给 sparse/lockdep 的类型标记，表示这个指针必须通过 RCU API 访问；
 *       它不会改变运行时对象的布局，也不会额外分配内存。
 *
 *   rcu_read_lock() / rcu_read_unlock()
 *       标记 RCU 读侧临界区的开始和结束。写者替换指针后，已经进入这个
 *       临界区的读者仍然可以安全使用旧对象。读侧必须尽量短，不能睡眠。
 *
 *   rcu_dereference(pointer)
 *       读者获取 __rcu 指针的标准方式。它包含正确的编译器/CPU 访问顺序，
 *       确保读者先看到指针，再读取对象内容。
 *
 *   rcu_dereference_protected(pointer, condition)
 *       写者或已经持有其他保护措施时读取 RCU 指针。condition 用来告诉
 *       lockdep：当前上下文为什么可以安全地直接读取这个指针。本例只有
 *       一个 writer，所以使用恒真的条件 1。
 *
 *   rcu_assign_pointer(pointer, value)
 *       发布一个新对象。它保证新对象已经初始化完成后，才让新读者看到
 *       新指针；不能用普通赋值替代。
 *
 *   RCU_INIT_POINTER(pointer, value)
 *       初始化或清空 RCU 指针时使用。它不承担“向并发读者发布新对象”的
 *       同步职责，所以不能把它替代 rcu_assign_pointer() 用在普通更新路径。
 *
 *   call_rcu(&object->rcu, callback)
 *       异步安排回收。等当前 RCU 读者全部离开后，内核调用 callback；回调
 *       不能睡眠。本例的 callback 用 container_of() 找回对象并 kfree()。
 *
 *   synchronize_rcu()
 *       同步等待一个 RCU 宽限期结束。调用者会睡眠，返回后保证调用前已经
 *       进入的读侧临界区都已结束，适合模块退出等进程上下文。
 *
 *   rcu_barrier()
 *       等待本 CPU/其他 CPU 上已经通过 call_rcu() 排队的回调全部执行完；
 *       它不是等待读者，而是等待回收回调，模块退出时用于确保回调不会再
 *       访问即将卸载的模块代码。
 *
 *   kfree_rcu(object, rcu_member)
 *       call_rcu() + 一个简单 kfree 回调的快捷写法。本例故意使用 call_rcu()
 *       自己写回调，方便观察宽限期结束的时刻。
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define MODULE_NAME "si_rcu19"

struct si_rcu_value {
	unsigned int version;
	unsigned int payload;
	struct rcu_head rcu;
};

/* __rcu 用于提醒 sparse/lockdep：这个指针必须用 RCU API 访问。 */
static struct si_rcu_value __rcu *current_value;

static unsigned int update_interval_ms = 1000;
module_param(update_interval_ms, uint, 0644);
MODULE_PARM_DESC(update_interval_ms, "Writer update interval in milliseconds");

static unsigned int reader_interval_ms = 500;
module_param(reader_interval_ms, uint, 0644);
MODULE_PARM_DESC(reader_interval_ms, "Reader interval in milliseconds");

/* 0 表示一直更新；非 0 表示更新指定次数后停止 writer。 */
static unsigned int updates;
module_param(updates, uint, 0644);
MODULE_PARM_DESC(updates, "Number of updates, or 0 for unlimited updates");

static struct task_struct *reader_task;
static struct delayed_work writer_work;
static unsigned int update_count;

static void si_rcu19_free(struct rcu_head *rcu)
{
	struct si_rcu_value *old;

	old = container_of(rcu, struct si_rcu_value, rcu);
	pr_info(MODULE_NAME ": grace period ended, freeing version=%u payload=%u\n",
		old->version, old->payload);
	kfree(old);
}

static void si_rcu19_writer(struct work_struct *work)
{
	struct si_rcu_value *new_value;
	struct si_rcu_value *old_value;

	if (updates && update_count >= updates)
		return;

	new_value = kmalloc(sizeof(*new_value), GFP_KERNEL);
	if (!new_value) {
		pr_err(MODULE_NAME ": failed to allocate new RCU value\n");
		return;
	}

	new_value->version = ++update_count;
	new_value->payload = new_value->version * 100;

	/* 只有这个 writer 修改指针，所以这里满足 protected 条件。 */
	old_value = rcu_dereference_protected(current_value, 1);

	/* 发布新对象：之后新进入的读者会看到 new_value。 */
	rcu_assign_pointer(current_value, new_value);

	pr_info(MODULE_NAME ": writer published version=%u payload=%u, "
		"old_version=%u\n", new_value->version, new_value->payload,
		old_value ? old_value->version : 0);

	/* 旧对象不能马上 kfree，要等已有读者离开读侧临界区。 */
	if (old_value)
		call_rcu(&old_value->rcu, si_rcu19_free);

	if (!updates || update_count < updates)
		schedule_delayed_work(&writer_work,
			msecs_to_jiffies(update_interval_ms));
}

static int si_rcu19_reader(void *unused)
{
	while (!kthread_should_stop()) {
		struct si_rcu_value *value;
		unsigned int version = 0;
		unsigned int payload = 0;
		bool found = false;

		/* 允许多个 reader 同时进入；这里不能睡眠。 */
		rcu_read_lock();
		value = rcu_dereference(current_value);
		if (value) {
			/* 必须在解锁前复制字段，而不是把 value 带到锁外。 */
			version = value->version;
			payload = value->payload;
			found = true;
		}
		rcu_read_unlock();

		if (found)
			pr_info(MODULE_NAME ": reader saw version=%u payload=%u "
				"current=%s pid=%d\n", version, payload,
				current->comm, current->pid);

		if (reader_interval_ms)
			msleep_interruptible(reader_interval_ms);
		else
			cond_resched();
	}

	return 0;
}

static int __init si_rcu19_init(void)
{
	struct si_rcu_value *initial;

	if (!update_interval_ms)
		update_interval_ms = 1;

	initial = kmalloc(sizeof(*initial), GFP_KERNEL);
	if (!initial)
		return -ENOMEM;

	initial->version = 0;
	initial->payload = 0;
	RCU_INIT_POINTER(current_value, initial);

	INIT_DELAYED_WORK(&writer_work, si_rcu19_writer);

	reader_task = kthread_run(si_rcu19_reader, NULL, MODULE_NAME "-reader");
	if (IS_ERR(reader_task)) {
		int ret = PTR_ERR(reader_task);

		RCU_INIT_POINTER(current_value, NULL);
		kfree(initial);
		return ret;
	}

	pr_info(MODULE_NAME ": loaded: reader_interval_ms=%u "
		"update_interval_ms=%u updates=%u\n", reader_interval_ms,
		update_interval_ms, updates);
	schedule_delayed_work(&writer_work, 0);
	return 0;
}

static void __exit si_rcu19_exit(void)
{
	struct si_rcu_value *last_value;

	/* 先停止 writer，保证不会再发布新对象或安排新的 work。 */
	cancel_delayed_work_sync(&writer_work);

	/* 再停止 reader，保证模块代码不会继续执行。 */
	if (reader_task)
		kthread_stop(reader_task);

	/* 断开指针，等待仍在读侧临界区中的 reader，然后释放最后一个对象。 */
	last_value = rcu_dereference_protected(current_value, 1);
	RCU_INIT_POINTER(current_value, NULL);
	synchronize_rcu();
	kfree(last_value);

	/* 等待之前 call_rcu() 安排的旧对象回收回调全部执行完。 */
	rcu_barrier();
	pr_info(MODULE_NAME ": unloaded after %u updates\n", update_count);
}

module_init(si_rcu19_init);
module_exit(si_rcu19_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI kernel learning");
MODULE_DESCRIPTION("Lesson 19: Read-Copy-Update (RCU)");
