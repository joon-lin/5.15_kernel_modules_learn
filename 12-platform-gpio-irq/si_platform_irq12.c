/*
 * Lesson 12：设备树 + platform_driver + GPIO 中断
 *
 * 这个模块对应 GJ3 设备树中的节点：
 *
 *     compatible = "si,lesson12-button";
 *     gpios = <&gpiof 5 GPIO_ACTIVE_HIGH>;
 *
 * 驱动不直接填写 Linux IRQ 号，也不把 GPIOF5 写死在 C 代码中。
 * 硬件信息由设备树提供，驱动在 probe() 中读取这些信息。
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>

#define DRIVER_NAME "si_platform_irq12"

struct si_lesson12_data {
	/* 保存设备对象，供 dev_info()/dev_err() 输出日志。 */
	struct device *dev;

	/* 设备树中的 gpios 属性对应的 GPIO 描述符。 */
	struct gpio_desc *button;

	/* gpiod_to_irq() 返回的 Linux 逻辑 IRQ 号。 */
	int irq;

	/* 只用于观察中断线程被触发了多少次。 */
	unsigned int event_count;
};

/*
 * 顶半部：仍然必须按硬中断上下文编写。
 *
 * 即使当前 GJ3 使用 PREEMPT_RT，也不要在这里调用：
 *
 *     msleep()、mutex_lock()、schedule()
 *
 * 这里暂时没有清除外部芯片的中断状态，只是教学演示，因此直接唤醒
 * 线程函数。真实设备驱动通常需要先读取并清除设备的中断状态。
 */
static irqreturn_t si_lesson12_irq_top(int irq, void *dev_id)
{
	struct si_lesson12_data *data = dev_id;

	dev_info(data->dev, "top half: irq=%d current=%s\n",
		 irq, current->comm);

	/* 告诉内核：请调度下面的 threaded handler。 */
	return IRQ_WAKE_THREAD;
}

/*
 * 线程化中断处理函数：运行在 irq/<number>-si_platform_irq12 线程中。
 *
 * 这里属于进程上下文，可以使用可能睡眠的 GPIO 读取 API。
 */
static irqreturn_t si_lesson12_irq_thread(int irq, void *dev_id)
{
	struct si_lesson12_data *data = dev_id;
	int value;

	/*
	 * GPIO 控制器有可能通过总线访问，读取操作可能睡眠，所以必须使用
	 * gpiod_get_value_cansleep()，不能把它放到顶半部中。
	 */
	value = gpiod_get_value_cansleep(data->button);
	if (value < 0) {
		dev_err(data->dev, "failed to read button GPIO: %d\n", value);
		return IRQ_HANDLED;
	}

	data->event_count++;
	dev_info(data->dev,
		 "thread: irq=%d value=%d count=%u current=%s pid=%d\n",
		 irq, value, data->event_count, current->comm, current->pid);

	return IRQ_HANDLED;
}

/*
 * probe() 是 platform_driver 和设备树设备真正建立联系的地方。
 *
 * 当设备树中的 compatible 与下面的 of_match_table 匹配时，内核会调用
 * 这个函数。通常发生在：
 *
 *     insmod si_platform_irq12.ko
 *
 * 之后；如果驱动是内建的，也可能在系统启动时调用。
 */
static int si_lesson12_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct si_lesson12_data *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;

	/*
	 * NULL 表示读取名为 "gpios" 的属性：
	 *
	 *     gpios = <&gpiof 5 GPIO_ACTIVE_HIGH>;
	 *
	 * GPIOF5 这个硬件细节因此留在设备树，而不是驱动源码中。
	 */
	data->button = devm_gpiod_get(dev, NULL, GPIOD_IN);
	if (IS_ERR(data->button)) {
		ret = PTR_ERR(data->button);
		dev_err(dev, "failed to get button GPIO: %d\n", ret);
		return ret;
	}

	/*
	 * 从 GPIO 描述符获取对应的 Linux IRQ。
	 * STM32 GPIO 驱动会把 GPIOF5 映射到 GPIO IRQ domain，再连接到
	 * EXTI 和 GIC；驱动不需要自己填写 EXTI 硬件号或 Linux IRQ 号。
	 */
	data->irq = gpiod_to_irq(data->button);
	if (data->irq < 0) {
		dev_err(dev, "failed to get GPIO IRQ: %d\n",
			data->irq);
		return data->irq;
	}

	platform_set_drvdata(pdev, data);

	/*
	 * devm_request_threaded_irq() 是设备管理版本：
	 *
	 * - probe() 成功后申请 IRQ；
	 * - remove() 或驱动卸载时，内核自动释放 IRQ；
	 * - IRQF_TRIGGER_RISING 表示按键按下时使用上升沿；
	 * - IRQF_ONESHOT 在线程函数执行期间屏蔽该 IRQ，避免重复进入。
	 */
	ret = devm_request_threaded_irq(dev,
					data->irq,
					si_lesson12_irq_top,
					si_lesson12_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					DRIVER_NAME,
					data);
	if (ret) {
		dev_err(dev, "failed to request IRQ %d: %d\n",
			data->irq, ret);
		return ret;
	}

	dev_info(dev, "probed: Linux IRQ=%d, GPIOF5 rising-edge\n",
		 data->irq);
	return 0;
}

/*
 * 因为使用 devm_* 资源管理 API，remove() 不需要手动 free_irq() 或
 * gpiod_put()。这里保留函数，是为了观察 platform_driver 的生命周期。
 */
static int si_lesson12_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "removed\n");
	return 0;
}

static const struct of_device_id si_lesson12_of_match[] = {
	{ .compatible = "si,lesson12-button" },
	{ }
};
MODULE_DEVICE_TABLE(of, si_lesson12_of_match);

static struct platform_driver si_lesson12_driver = {
	.probe = si_lesson12_probe,
	.remove = si_lesson12_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = si_lesson12_of_match,
	},
};

module_platform_driver(si_lesson12_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 12: device tree platform driver with GPIO IRQ");
