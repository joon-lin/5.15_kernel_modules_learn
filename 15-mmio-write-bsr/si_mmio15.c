/*
 * ============================================================================
 * Lesson 15：真实 GPIOH11 LED 的 MMIO 写寄存器实验
 * ============================================================================
 *
 * 本课操作 GJ3 上连接 LED 的 GPIOH11。TF-A 已把 PH11 配置成 GPIO 输出，
 * 因此本课不改 MODER，只通过 GPIOH_BSRR 改变输出锁存值。
 *
 * 一、寄存器地址
 *
 *     GPIOH 基地址 = 0x50009000
 *     MODER 偏移   = 0x00，地址 0x50009000
 *     ODR 偏移     = 0x14，地址 0x50009014
 *     BSRR 偏移    = 0x18，地址 0x50009018
 *     PH11        = bit 11
 *
 * 二、BSRR 的写入值
 *
 *     BSRR bit 0  ~ bit 15   写 1：设置对应 ODR 位
 *     BSRR bit 16 ~ bit 31   写 1：清除对应 ODR 位
 *
 *     设置 PH11：0x00000800 = BIT(11)
 *     清除 PH11：0x08000000 = BIT(11) << 16
 *
 * 三、为什么用 BSRR？
 *
 * 直接修改 ODR 通常需要 read-modify-write，可能覆盖其他 GPIO 位。BSRR
 * 直接表达“设置某一位”或“清除某一位”，由硬件完成原子操作。
 * BSRR 是写入式寄存器，不应该读取它；写入后读取 ODR 验证结果。
 *
 * 四、如何操作？
 *
 *     echo 1 > /sys/module/si_mmio15/parameters/ph11
 *
 * 驱动执行：
 *
 *     writel(0x00000800, gpioh_base + 0x18);
 *
 *     echo 0 > /sys/module/si_mmio15/parameters/ph11
 *
 * 驱动执行：
 *
 *     writel(0x08000000, gpioh_base + 0x18);
 *
 * 每次操作都会打印：写入的物理地址、寄存器偏移、写入值、操作含义，
 * 随后读取 GPIOH_ODR 打印验证值。读取 ph11 参数还会读取 MODER，确认
 * PH11 当前确实是 GPIO 输出模式。
 *
 * 五、LED 亮灭方向
 *
 * LED 可能是高电平点亮，也可能是低电平点亮，取决于原理图连接方式。
 * 因此 echo 1 不一定代表“LED 亮”，日志中的 PH11_latch 表示 GPIO 输出
 * 锁存值；最终以实物 LED 状态为准。
 *
 * 六、安全边界
 *
 * 本课不修改 MODER、OTYPER、OSPEEDR、PUPDR。若 MODER 显示 PH11 不是
 * 输出模式，驱动会拒绝写 BSRR，避免写入对 LED 没有意义或误操作引脚。
 * ============================================================================
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#define DRIVER_NAME             "si_mmio15"
#define GPIOH_BASE_PHYS         0x50009000U
#define GPIOH_MAP_SIZE          0x400U
#define GPIOH_MODER_OFFSET      0x00U
#define GPIOH_ODR_OFFSET        0x14U
#define GPIOH_BSRR_OFFSET       0x18U
#define GPIOH_MODER_PHYS        (GPIOH_BASE_PHYS + GPIOH_MODER_OFFSET)
#define GPIOH_ODR_PHYS          (GPIOH_BASE_PHYS + GPIOH_ODR_OFFSET)
#define GPIOH_BSRR_PHYS         (GPIOH_BASE_PHYS + GPIOH_BSRR_OFFSET)
#define GPIOH11_BIT             11U
#define GPIOH11_MODE_MASK       (0x3U << (GPIOH11_BIT * 2U))
#define GPIOH11_OUTPUT_MODE     (0x1U << (GPIOH11_BIT * 2U))
#define GPIOH11_SET_VALUE       ((u32)BIT(GPIOH11_BIT))
#define GPIOH11_CLEAR_VALUE     ((u32)(BIT(GPIOH11_BIT) << 16))

static void __iomem *gpioh_base;

static int ph11_get(char *buffer, const struct kernel_param *kp)
{
	u32 moder;
	u32 odr;
	unsigned int mode;
	unsigned int output_mode;
	unsigned int ph11_latch;

	(void)kp;

	if (!gpioh_base)
		return -ENODEV;

	moder = readl(gpioh_base + GPIOH_MODER_OFFSET);
	odr = readl(gpioh_base + GPIOH_ODR_OFFSET);
	mode = (moder & GPIOH11_MODE_MASK) >> (GPIOH11_BIT * 2U);
	output_mode = mode == 1U;
	ph11_latch = !!(odr & BIT(GPIOH11_BIT));

	pr_info(DRIVER_NAME
		": readl phys=0x%08x offset=0x%02x register=GPIOH_MODER "
		"value=0x%08x PH11_mode=%u output=%u\n",
		GPIOH_MODER_PHYS, GPIOH_MODER_OFFSET, moder, mode, output_mode);
	pr_info(DRIVER_NAME
		": readl phys=0x%08x offset=0x%02x register=GPIOH_ODR "
		"value=0x%08x PH11_latch=%u\n",
		GPIOH_ODR_PHYS, GPIOH_ODR_OFFSET, odr, ph11_latch);

	return scnprintf(buffer, PAGE_SIZE,
			 "GPIOH_ODR[0x%02x] = 0x%08x, PH11_latch = %u, output = %u\n",
			 GPIOH_ODR_OFFSET, odr, ph11_latch, output_mode);
}

static int ph11_set(const char *value, const struct kernel_param *kp)
{
	int requested;
	u32 moder;
	u32 bsrr_value;
	u32 odr;

	(void)kp;

	if (!gpioh_base)
		return -ENODEV;
	if (kstrtoint(value, 0, &requested))
		return -EINVAL;

	moder = readl(gpioh_base + GPIOH_MODER_OFFSET);
	if ((moder & GPIOH11_MODE_MASK) != GPIOH11_OUTPUT_MODE) {
		pr_err(DRIVER_NAME
		       ": PH11 is not GPIO output: MODER phys=0x%08x value=0x%08x\n",
		       GPIOH_MODER_PHYS, moder);
		return -EPERM;
	}

	if (requested == 1) {
		bsrr_value = GPIOH11_SET_VALUE;
		pr_info(DRIVER_NAME
			": writel phys=0x%08x offset=0x%02x value=0x%08x "
			"register=GPIOH_BSRR operation=SET PH11(bit11)\n",
			GPIOH_BSRR_PHYS, GPIOH_BSRR_OFFSET, bsrr_value);
	} else if (requested == 0) {
		bsrr_value = GPIOH11_CLEAR_VALUE;
		pr_info(DRIVER_NAME
			": writel phys=0x%08x offset=0x%02x value=0x%08x "
			"register=GPIOH_BSRR operation=CLEAR PH11(bit11)\n",
			GPIOH_BSRR_PHYS, GPIOH_BSRR_OFFSET, bsrr_value);
	} else {
		pr_err(DRIVER_NAME ": only 0 or 1 is accepted\n");
		return -EINVAL;
	}

	writel(bsrr_value, gpioh_base + GPIOH_BSRR_OFFSET);
	odr = readl(gpioh_base + GPIOH_ODR_OFFSET);

	pr_info(DRIVER_NAME
		": readl phys=0x%08x offset=0x%02x register=GPIOH_ODR "
		"after-BSRR value=0x%08x PH11_latch=%u\n",
		GPIOH_ODR_PHYS, GPIOH_ODR_OFFSET, odr,
		(unsigned int)!!(odr & BIT(GPIOH11_BIT)));

	return 0;
}

static const struct kernel_param_ops ph11_param_ops = {
	.get = ph11_get,
	.set = ph11_set,
};

module_param_cb(ph11, &ph11_param_ops, NULL, 0644);
MODULE_PARM_DESC(ph11, "Set/clear GPIOH11 ODR latch through BSRR");

static int __init si_mmio15_init(void)
{
	gpioh_base = ioremap(GPIOH_BASE_PHYS, GPIOH_MAP_SIZE);
	if (!gpioh_base) {
		pr_err(DRIVER_NAME ": ioremap failed phys=0x%08x size=0x%x\n",
		       GPIOH_BASE_PHYS, GPIOH_MAP_SIZE);
		return -ENOMEM;
	}

	pr_info(DRIVER_NAME
		": mapped GPIOH phys=0x%08x size=0x%x, "
		"MODER=0x%08x ODR=0x%08x BSRR=0x%08x PH11=bit11\n",
		GPIOH_BASE_PHYS, GPIOH_MAP_SIZE, GPIOH_MODER_PHYS,
		GPIOH_ODR_PHYS, GPIOH_BSRR_PHYS);
	pr_info(DRIVER_NAME
		": use echo 1/0 > /sys/module/%s/parameters/ph11\n",
		DRIVER_NAME);

	return 0;
}

static void __exit si_mmio15_exit(void)
{
	if (gpioh_base) {
		writel(GPIOH11_CLEAR_VALUE, gpioh_base + GPIOH_BSRR_OFFSET);
		pr_info(DRIVER_NAME
			": writel phys=0x%08x offset=0x%02x value=0x%08x "
			"operation=CLEAR PH11 during unload\n",
			GPIOH_BSRR_PHYS, GPIOH_BSRR_OFFSET, GPIOH11_CLEAR_VALUE);
		iounmap(gpioh_base);
		gpioh_base = NULL;
	}

	pr_info(DRIVER_NAME ": unmapped GPIOH\n");
}

module_init(si_mmio15_init);
module_exit(si_mmio15_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 15: GPIOH11 LED BSRR MMIO writes");
