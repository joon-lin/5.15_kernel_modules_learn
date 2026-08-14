# Lesson 14：真实 GPIO 寄存器与 MMIO

这一课直接读取 GJ3 的真实 GPIOF5 输入寄存器。模块只读 `GPIOF_IDR`，不
修改任何 GPIO 配置或输出寄存器。

## 编译

进入 Yocto 内核 devshell：

```sh
bitbake -c devshell virtual/kernel

cd /home/winterforest/Yocto/si-stm/kernel_modules/14-platform-resource-mmio
make
```

## 板上实验

加载模块：

```sh
insmod si_mmio14.ko
```

查看初始化日志：

```sh
dmesg | tail -n 5
```

读取一次真实寄存器：

```sh
cat /sys/module/si_mmio14/parameters/idr
```

同时查看日志：

```sh
dmesg | tail -n 3
```

模块操作的是：

```text
GPIOF 基地址  0x50007000
IDR 偏移      0x10
IDR 地址      0x50007010
PF5           bit 5
```

按住或松开按钮后重复 `cat`，观察 `PF5(bit5)` 的变化。

卸载模块：

```sh
rmmod si_mmio14
```

## 本课 API

```c
platform_get_resource()
devm_ioremap_resource()
devm_platform_ioremap_resource()
ioremap()
iounmap()
readl()
writel()
resource_size()
```

核心原则：

```text
硬件手册物理地址 → ioremap → readl/writel
```

本课只执行 `readl()`。不要直接对硬件物理地址进行普通 C 指针解引用，
也不要在没有确认数据手册和原理图前使用 `writel()`。
