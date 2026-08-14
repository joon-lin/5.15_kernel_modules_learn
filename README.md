# Linux 内核 API 学习仓库

通过 Yocto devshell 学习和编译 Linux 内核模块。

## 课程目录

- `01/`：模块加载与卸载
- `02-module-param/`：模块参数
- `03-memory/`：内核内存分配与释放
- `04-char-device/`：字符设备与用户空间读写，包含 `user/si_char_test.c` 测试程序
- `05-mutex/`：互斥锁与并发访问
- `06-spinlock/`：自旋锁与短临界区
- `07-waitqueue/`：等待队列与阻塞式字符设备
- `08-poll/`：poll 与非阻塞字符设备
- `09-workqueue/`：workqueue 与 delayed_work
- `10-tasklet-softirq/`：tasklet 与 softirq
- `11-irq/`：硬件中断、threaded IRQ 与 request_irq 对比
- `12-platform-gpio-irq/`：设备树、platform_driver 与 GPIO 中断
- `13-sysfs/`：sysfs 属性与内核对象的用户态接口
- `14-platform-resource-mmio/`：真实 GPIO 寄存器与 MMIO 读取
- `15-mmio-write-bsr/`：GPIOH11 LED、MMIO 写入与 BSRR 原子置位/复位
- `16-ioctl/`：字符设备 ioctl 与用户态控制接口
- `17-kthread/`：创建、运行和安全停止内核线程
- `18-timer/`：PREEMPT_RT 下的硬中断高精度定时器与延迟测量
- `19-rcu/`：RCU 读侧、写侧和宽限期回收
- `20-dma/`：DMA 缓冲区、DMA 映射与 Cache 同步
- `21-i2c/`：设备树匹配、I2C 通信，以及 misc/cdev 两种字符设备写法
- `22-spi/`：设备树匹配、SPI 片选、SPI 传输与字符设备

## 编译

进入 Yocto 内核 devshell：

```sh
source layers/openembedded-core/oe-init-build-env build-gj3_2
bitbake virtual/kernel
bitbake -c devshell virtual/kernel
```

进入课程目录编译：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/01
make
```

每个课程目录都是独立的模块工程，编译生成的文件会被 `.gitignore` 忽略。

Lesson 4 的用户态测试程序：

```sh
# 先在当前 shell 中加载 SDK 环境
. /opt/st/stm32mp15-eval-mx-gj3/4.0.4-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi

cd /home/winterforest/Yocto/si-stm/kernel_modules/04-char-device/user
make
```

Makefile 直接使用已加载的 `CC` 环境变量。

Lesson 7 的用户态测试程序：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/07-waitqueue/user
make
```

Lesson 8 的用户态测试程序：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/08-poll/user
make
```
