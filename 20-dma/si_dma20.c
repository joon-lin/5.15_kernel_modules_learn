/*
 * Lesson 20：真正的 DMAengine 内存到内存传输
 *
 * 这次不是只申请 DMA 缓冲区，而是实际申请 STM32 的 DMAengine 通道，
 * 让 DMA 控制器把 source 缓冲区复制到 destination 缓冲区，然后比较两块
 * 内存，验证硬件传输确实完成。
 *
 * 传输流程：
 *
 *   DMA_MEMCPY 能力
 *       ↓
 *   dma_request_chan_by_mask() 申请真实 DMA 通道
 *       ↓
 *   dma_alloc_coherent() 申请源/目标 DMA 缓冲区
 *       ↓
 *   dmaengine_prep_dma_memcpy() 准备硬件描述符
 *       ↓
 *   dmaengine_submit() 提交描述符
 *       ↓
 *   dma_async_issue_pending() 启动传输
 *       ↓
 *   DMA 完成中断 → callback → complete()
 *       ↓
 *   CPU 比较 source 和 destination
 *
 * GJ3 的 STM32MP1 内核中，STM32 MDMA 驱动提供 DMA_MEMCPY 能力，因此这个
 * 示例可以进行真实的内存到内存 DMA。它不需要 SPI、UART 或外部接线，但它
 * 使用的是芯片里的真实 MDMA 控制器和 DMA 完成中断。
 *
 * 本课涉及的 DMAengine API：
 *
 *   dma_cap_mask_t / dma_cap_zero() / dma_cap_set()
 *       描述调用者需要的 DMA 能力。本例只请求 DMA_MEMCPY。
 *
 *   dma_request_chan_by_mask(&mask)
 *       根据能力申请一个空闲 DMA 通道。成功后这个通道归当前驱动独占，
 *       使用完必须 dma_release_channel()。
 *
 *   dma_alloc_coherent(dev, size, &dma_handle, gfp)
 *       同时返回 CPU 虚拟地址和设备使用的 DMA 地址。本例用它申请源和
 *       目标缓冲区，因此不需要额外的 streaming sync。
 *
 *   dmaengine_prep_dma_memcpy(chan, dst, src, len, flags)
 *       创建一次内存到内存复制的 DMA 描述符。这里的 src/dst 必须是 DMA
 *       地址，不是 CPU 虚拟地址。
 *
 *   DMA_PREP_INTERRUPT / DMA_CTRL_ACK
 *       请求传输完成中断，并允许 DMAengine 回收描述符。
 *
 *   dmaengine_submit(desc)
 *       把描述符放入 DMAengine 队列，返回 cookie。它此时还不一定已经开始。
 *
 *   dma_async_issue_pending(chan)
 *       通知 DMA 控制器开始执行已经提交的描述符。
 *
 *   desc->callback / desc->callback_param
 *       传输完成后由 DMA 中断路径调用。本例的回调只调用 complete()，不做
 *       睡眠或耗时工作。
 *
 *   dmaengine_terminate_sync(chan)
 *       停止并等待通道上的传输，用于超时和卸载路径，确保释放缓冲区前 DMA
 *       不再访问它们。
 *
 *   dma_free_coherent() / dma_release_channel()
 *       分别释放 DMA 缓冲区和 DMA 通道。
 *
 * 注意：
 *   DMA 地址不是 CPU 虚拟地址，也不应该用 virt_to_phys() 自己转换。真实
 *   外设驱动还要把 DMA 地址写入外设寄存器，并根据设备手册配置 DMA 请求；
 *   本例使用 MDMA 的软件触发内存复制，所以不需要外部设备。
 */

#include <linux/completion.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#define MODULE_NAME "si_dma20"

static unsigned int transfer_size = 4096;
module_param(transfer_size, uint, 0444);
MODULE_PARM_DESC(transfer_size, "DMA transfer size in bytes");

static unsigned int transfers = 1;
module_param(transfers, uint, 0444);
MODULE_PARM_DESC(transfers, "Number of DMA memcpy transfers");

static unsigned int timeout_ms = 1000;
module_param(timeout_ms, uint, 0444);
MODULE_PARM_DESC(timeout_ms, "Timeout for one DMA transfer in milliseconds");

static struct dma_chan *dma_chan;
static struct device *dma_device;
static void *source_cpu_addr;
static void *destination_cpu_addr;
static dma_addr_t source_dma_addr;
static dma_addr_t destination_dma_addr;

static void si_dma20_complete(void *arg)
{
	struct completion *done = arg;

	/* 完成回调可能运行在 DMA 中断上下文，不能睡眠。 */
	complete(done);
}

static void si_dma20_cleanup(void)
{
	if (dma_chan) {
		/* 释放缓冲区前，先确保 DMA 不再访问它们。 */
		dmaengine_terminate_sync(dma_chan);
	}

	if (source_cpu_addr) {
		dma_free_coherent(dma_device, transfer_size, source_cpu_addr,
			source_dma_addr);
		source_cpu_addr = NULL;
	}

	if (destination_cpu_addr) {
		dma_free_coherent(dma_device, transfer_size,
			destination_cpu_addr, destination_dma_addr);
		destination_cpu_addr = NULL;
	}

	if (dma_chan) {
		dma_release_channel(dma_chan);
		dma_chan = NULL;
		dma_device = NULL;
	}
}

static int si_dma20_run_transfer(unsigned int number)
{
	struct completion done;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	unsigned long timeout;
	unsigned char *source;
	unsigned int i;
	int ret;

	source = source_cpu_addr;
	for (i = 0; i < transfer_size; i++)
		source[i] = (unsigned char)(i + number);
	memset(destination_cpu_addr, 0, transfer_size);

	init_completion(&done);
	desc = dmaengine_prep_dma_memcpy(dma_chan, destination_dma_addr,
		source_dma_addr, transfer_size,
		DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!desc) {
		pr_err(MODULE_NAME ": prep_dma_memcpy failed for transfer %u\n",
			number);
		return -EIO;
	}

	desc->callback = si_dma20_complete;
	desc->callback_param = &done;
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret) {
		pr_err(MODULE_NAME ": dmaengine_submit failed for transfer %u: %d\n",
			number, ret);
		return ret;
	}

	pr_info(MODULE_NAME ": transfer %u submitted: src=%pad dst=%pad size=%u\n",
		number, &source_dma_addr, &destination_dma_addr, transfer_size);
	dma_async_issue_pending(dma_chan);

	timeout = wait_for_completion_timeout(&done,
		msecs_to_jiffies(timeout_ms));
	if (!timeout) {
		pr_err(MODULE_NAME ": transfer %u timed out after %u ms\n",
			number, timeout_ms);
		dmaengine_terminate_sync(dma_chan);
		return -ETIMEDOUT;
	}

	if (memcmp(source_cpu_addr, destination_cpu_addr, transfer_size)) {
		pr_err(MODULE_NAME ": transfer %u data mismatch\n", number);
		return -EIO;
	}

	pr_info(MODULE_NAME ": transfer %u completed by DMA, data verified\n",
		number);
	return 0;
}

static int __init si_dma20_init(void)
{
	dma_cap_mask_t mask;
	unsigned int i;
	int ret;

	if (!transfer_size || !transfers || !timeout_ms)
		return -EINVAL;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	dma_chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(dma_chan)) {
		ret = PTR_ERR(dma_chan);
		pr_err(MODULE_NAME ": no DMA_MEMCPY channel: %d\n", ret);
		dma_chan = NULL;
		return ret;
	}

	dma_device = dma_chan->device->dev;
	pr_info(MODULE_NAME ": using channel %s on device %s\n",
		dma_chan_name(dma_chan), dev_name(dma_device));

	source_cpu_addr = dma_alloc_coherent(dma_device, transfer_size,
		&source_dma_addr, GFP_KERNEL);
	if (!source_cpu_addr) {
		ret = -ENOMEM;
		pr_err(MODULE_NAME ": source dma_alloc_coherent failed\n");
		goto err_cleanup;
	}

	destination_cpu_addr = dma_alloc_coherent(dma_device, transfer_size,
		&destination_dma_addr, GFP_KERNEL);
	if (!destination_cpu_addr) {
		ret = -ENOMEM;
		pr_err(MODULE_NAME ": destination dma_alloc_coherent failed\n");
		goto err_cleanup;
	}

	pr_info(MODULE_NAME ": source cpu=%p dma=%pad, destination cpu=%p dma=%pad\n",
		source_cpu_addr, &source_dma_addr, destination_cpu_addr,
		&destination_dma_addr);

	for (i = 0; i < transfers; i++) {
		ret = si_dma20_run_transfer(i + 1);
		if (ret)
			goto err_cleanup;
	}

	pr_info(MODULE_NAME ": all %u real DMA transfers completed\n", transfers);
	return 0;

err_cleanup:
	si_dma20_cleanup();
	return ret;
}

static void __exit si_dma20_exit(void)
{
	si_dma20_cleanup();
	pr_info(MODULE_NAME ": unloaded; DMA channel and buffers released\n");
}

module_init(si_dma20_init);
module_exit(si_dma20_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI kernel learning");
MODULE_DESCRIPTION("Lesson 20: real DMAengine memory-to-memory transfer");
