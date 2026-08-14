# Lesson 18：PREEMPT_RT 下的硬中断定时器 `hrtimer`

本课使用高精度定时器 `hrtimer` 创建一个周期性定时器，并使用
`HRTIMER_MODE_REL_PINNED_HARD`。即使内核启用了 `CONFIG_PREEMPT_RT`，
回调仍然在硬中断上下文执行，并固定在启动定时器时所在的 CPU。

## 编译

先进入内核 devshell，再编译：

```sh
bitbake -c devshell virtual/kernel
cd /home/winterforest/Yocto/si-stm/kernel_modules/18-timer
make
```

## 板上实验

```sh
taskset -c 1 insmod si_timer18.ko period_ms=1 report_every=100
dmesg | tail -n 10
```

加载后先看日志：

```sh
dmesg | tail -n 5
```

每隔约 1 ms 会执行一次回调。默认只打印第 1 次和每 100 次中的 1 次，
例如：

```text
sample=1 late_ns=...
sample=100 late_ns=...
```

如果要固定到 CPU1，必须同时满足：

```sh
cat /sys/devices/system/cpu/online
cat /sys/devices/system/cpu/isolated
taskset -c 1 insmod si_timer18.ko period_ms=1 report_every=100
```

`taskset` 让 `hrtimer_start()` 在 CPU1 上执行，`PINNED` 则让定时器继续
留在这个 CPU 上。日志中的 `start_cpu=1` 和 `cpu=1` 可用于确认。

`late_ns` 是本次回调相对于预定到期时间晚了多少纳秒。卸载模块后会打印
整体统计：

```text
late_ns(min/avg/max)=.../.../...
jitter_range_ns=...
```

查看日志：

```sh
dmesg | tail -n 5
```

然后卸载：

```sh
rmmod si_timer18
```

卸载时，`hrtimer_cancel()` 会取消尚未到期的定时器，并等待正在执行的
回调结束，因此不会再调用已经卸载的模块代码。

## API 对照

```text
hrtimer_init()       初始化 hrtimer
hrtimer_start()      启动或重新安排定时器
hrtimer_forward_now()推进下一次相对当前到期时间
HRTIMER_MODE_REL     相对当前时间计时
HRTIMER_MODE_REL_PINNED_HARD 在 RT 下请求硬中断回调并固定 CPU
hrtimer_cancel()     删除并等待回调结束
```

本课的回调运行在硬中断上下文，绝对不能睡眠。不要调用 `msleep()`、
`mutex_lock()` 或 `wait_event()`。需要执行可能睡眠或耗时的工作时，应在
回调里调用 `schedule_work()`，再由 workqueue 完成真正的工作。

回调返回 `HRTIMER_RESTART` 才会继续触发；返回 `HRTIMER_NORESTART` 则只
执行一次。`hrtimer_forward_now()` 用于推进下一次到期时间。

## 如何理解延迟和抖动

```text
late_ns：这一次触发晚了多久
jitter：  max_late_ns - min_late_ns，延迟的波动范围
```

这测量的是“定时器到期到回调开始”的内核软件延迟，不是外部 GPIO 信号
到 CPU 进入 IRQ 的完整硬件延迟。不要在每次回调里打印日志，否则 printk
本身会改变被测系统；可以通过 `report_every=1` 观察每次样本，但结果会受
日志开销影响。

`hrtimer` 使用 `ktime_t`，精度高于基于 `jiffies` 的 `timer_list`。如果去掉
`HRTIMER_MODE_REL_PINNED_HARD`，RT 内核通常会把 hrtimer 放到线程化的 timer/softirq
上下文执行；本课特意保留它来观察原来的硬中断语义。
