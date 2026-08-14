# Lesson 17：内核线程 `kthread`

本课创建一个内核线程，让它周期性打印日志，并在模块卸载时安全退出。

## 编译

先进入内核 devshell，再编译：

```sh
bitbake -c devshell virtual/kernel
cd /home/winterforest/Yocto/si-stm/kernel_modules/17-kthread
make
```

## 板上实验

```sh
insmod si_kthread17.ko
dmesg | tail -n 10
```

默认每秒打印一次。也可以指定周期：

```sh
insmod si_kthread17.ko period_ms=200
```

查看线程：

```sh
ps -eLo pid,tid,comm | grep si_kthread17
```

卸载模块：

```sh
rmmod si_kthread17
dmesg | tail -n 10
```

## 本课重点

```text
kthread_run()          创建并启动内核线程
kthread_should_stop()  在线程函数中检查退出请求
kthread_stop()         请求退出、唤醒并等待线程结束
wait_event_*()          让线程睡眠，避免循环空转占用 CPU
```

`kthread_stop()` 不是强制杀死线程，而是请求线程自行返回。因此线程函数
必须在循环中检查 `kthread_should_stop()`。

内核线程没有用户空间地址空间，但仍然是可调度的 task，有自己的 PID、
`current` 和调度优先级。线程可以睡眠，但不能在持有自旋锁或处于中断上下文
时睡眠。
