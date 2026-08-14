# Lesson 12：设备树与 GPIO 中断

这一课把前面学过的 IRQ API 接到真实硬件上：

```text
设备树 compatible
    ↓
platform_driver.probe()
    ↓
devm_gpiod_get() + gpiod_to_irq()
    ↓
devm_request_threaded_irq()
    ↓
GPIOF5 上升沿
    ↓
中断线程函数
```

GJ3 临时设备树节点使用：

```text
GPIOF5 → EXTI5 → GIC → Linux IRQ
```

驱动源码不会写死 Linux IRQ 号。`gpiod_to_irq()` 得到的 IRQ 是
Linux 根据 GPIO IRQ domain 映射后的逻辑 IRQ。

## 编译

先进入 Yocto 内核 devshell：

```sh
bitbake -c devshell virtual/kernel
```

然后编译：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/12-platform-gpio-irq
make
```

## 板上实验

确认运行中的设备树已经包含节点：

```sh
ls /sys/firmware/devicetree/base/lesson12-button
```

复制并加载模块：

```sh
insmod si_platform_irq12.ko
dmesg -w
```

正常情况下会看到类似：

```text
si_platform_irq12: probed: Linux IRQ=xxx, GPIOF5 rising-edge
```

按下连接到 PF5 的按钮后，应该看到：

```text
si_platform_irq12: thread: irq=xxx value=1 count=1 current=irq/xxx-si_platform_irq12 pid=...
```

查看 IRQ 计数：

```sh
grep -Ei 'lesson12|gpio|exti' /proc/interrupts
```

卸载：

```sh
rmmod si_platform_irq12
```

如果 `insmod` 成功但没有 `probed` 日志，通常是当前启动的 DTB 没有
    `lesson12-button` 节点，或者 `compatible` 与驱动不一致。
