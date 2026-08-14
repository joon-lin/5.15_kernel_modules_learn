/*
 * ============================================================================
 * Lesson 14：真实 GPIO 寄存器读取实验
 * ============================================================================
 *
 * 本课直接读取 GJ3 上 GPIOF 的真实 MMIO 寄存器。
 * 当前只读 GPIOF5 的输入数据，不写任何寄存器，因此不会改变 GPIO 配置。
 *
 * 一、这次操作的硬件地址
 *
 * STM32MP1 设备树中 GPIOF 的资源是：
 *
 *     gpiof: gpio@50007000 {
 *         reg = <0x5000 0x400>;
 *     };
 *
 * 所以：
 *
 *     GPIOF 基地址       = 0x50007000
 *     GPIOF 映射大小      = 0x400
 *     IDR 寄存器偏移      = 0x10
 *     IDR 物理地址        = 0x50007010
 *     PF5 对应 IDR bit 5
 *
 * 二、GPIOF 常用寄存器
 *
 *     GPIOF + 0x00  MODER  模式配置寄存器
 *     GPIOF + 0x04  OTYPER 输出类型寄存器
 *     GPIOF + 0x08  OSPEEDR 输出速度寄存器
 *     GPIOF + 0x0c  PUPDR  上下拉配置寄存器
 *     GPIOF + 0x10  IDR    输入数据寄存器（本课只读）
 *     GPIOF + 0x14  ODR    输出数据寄存器
 *     GPIOF + 0x18  BSRR   原子置位/复位寄存器
 *
 * 三、为什么使用 ioremap()、readl()？
 *
 *     ioremap(0x50007000, 0x400)
 *         把硬件物理地址映射成内核使用的 __iomem 地址。
 *
 *     readl(base + 0x10)
 *         从 GPIOF_IDR 读取一个 32 位寄存器值。
 *
 * base 不是普通 RAM 指针，不能使用普通指针解引用，也不能使用 memcpy()
 * 访问。硬件寄存器必须通过 readl()/writel() 等 MMIO API 访问。
 *
 * 四、如何触发一次真实寄存器读取？
 *
 * 模块加载后会出现一个只读的模块参数：
 *
 *     /sys/module/si_mmio14/parameters/idr
 *
 * 执行：
 *
 *     cat /sys/module/si_mmio14/parameters/idr
 *
 * 会调用 idr_get()，执行一次：
 *
 *     value = readl(gpiof_base + 0x10);
 *
 * 同时在 dmesg 中打印：
 *
 *     物理地址、寄存器名称、偏移、完整寄存器值、PF5 bit 5 的值。
 *
 * 五、如何观察 PF5 的变化？
 *
 * 先执行：
 *
 *     cat /sys/module/si_mmio14/parameters/idr
 *
 * 再按住或松开按钮，重复执行同一命令。PF5 的原始电平变化会反映到
 * IDR bit 5：
 *
 *     (IDR value & BIT(5)) != 0  → PF5 高电平
 *     (IDR value & BIT(5)) == 0  → PF5 低电平
 *
 * 六、为什么本课不用 devm_platform_ioremap_resource()？
 *
 * GPIOF 的 0x50007000 区域已经由系统 GPIO/pinctrl 驱动使用。为了避免
 * 创建重复的 platform resource、与现有 GPIO 驱动争抢同一资源，本课只对
 * 已确认的地址建立只读映射，不申请资源所有权，也不写寄存器。
 *
 * 正式设备驱动通常仍应优先使用设备树 reg 和
 * devm_platform_ioremap_resource()。这里是为了安全地观察一个已经运行的
 * GPIO 控制器。
 *
 * 七、本课明确不会做什么？
 *
 * 本课不会写 MODER、ODR、BSRR 或其他寄存器。写错这些寄存器可能改变
 * GPIO 复用、输出电平，甚至影响正在使用的外设。确认数据手册和硬件原理图
 * 后，下一步再单独学习安全的 writel() 实验。
 * ============================================================================
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#define DRIVER_NAME            "si_mmio14"
#define GPIOF_BASE_PHYS        0x50007000
#define GPIOF_MAP_SIZE         0x400
#define GPIOF_IDR_OFFSET       0x10
#define GPIOF_IDR_PHYS         (GPIOF_BASE_PHYS + GPIOF_IDR_OFFSET)
#define GPIOF5_BIT             5

static void __iomem *gpiof_base;

/*
 * 读取 /sys/module/si_mmio14/parameters/idr 时执行。
 * 这是一个只读 module parameter，但它的 get 回调实际访问了硬件寄存器。
 */
static int idr_get(char *buffer, const struct kernel_param *kp)
{
	u32 value;
	unsigned int pf5;

	if (!gpiof_base)
		return -ENODEV;

	value = readl(gpiof_base + GPIOF_IDR_OFFSET);
	pf5 = !!(value & BIT(GPIOF5_BIT));

	pr_info(DRIVER_NAME
		": readl phys=0x%08x offset=0x%02x register=GPIOF_IDR "
		"value=0x%08x PF5(bit5)=%u\n",
		GPIOF_IDR_PHYS, GPIOF_IDR_OFFSET, value, pf5);

	return scnprintf(buffer, PAGE_SIZE,
			 "GPIOF_IDR[0x%02x] = 0x%08x, PF5(bit5) = %u\n",
			 GPIOF_IDR_OFFSET, value, pf5);
}

static const struct kernel_param_ops idr_param_ops = {
	.get = idr_get,
};

module_param_cb(idr, &idr_param_ops, NULL, 0444);
MODULE_PARM_DESC(idr, "Read GPIOF IDR and decode PF5");

static int __init si_mmio14_init(void)
{
	/* 只建立映射，不申请 GPIOF 区域所有权，也不写寄存器。 */
	gpiof_base = ioremap(GPIOF_BASE_PHYS, GPIOF_MAP_SIZE);
	if (!gpiof_base) {
		pr_err(DRIVER_NAME ": ioremap GPIOF failed: phys=0x%08x size=0x%x\n",
		       GPIOF_BASE_PHYS, GPIOF_MAP_SIZE);
		return -ENOMEM;
	}

	pr_info(DRIVER_NAME
		": mapped GPIOF phys=0x%08x size=0x%x, "
		"IDR phys=0x%08x offset=0x%02x\n",
		GPIOF_BASE_PHYS, GPIOF_MAP_SIZE,
		GPIOF_IDR_PHYS, GPIOF_IDR_OFFSET);
	pr_info(DRIVER_NAME
		": read /sys/module/%s/parameters/idr to read GPIOF_IDR\n",
		DRIVER_NAME);

	return 0;
}

static void __exit si_mmio14_exit(void)
{
	iounmap(gpiof_base);
	gpiof_base = NULL;
	pr_info(DRIVER_NAME ": unmapped GPIOF\n");
}

module_init(si_mmio14_init);
module_exit(si_mmio14_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 14: read real STM32MP1 GPIOF MMIO register");
