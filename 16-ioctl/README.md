# Lesson 16：ioctl

这一课学习字符设备的控制接口 `ioctl()`，并附带用户态测试程序。

## 编译

内核模块：

```sh
bitbake -c devshell virtual/kernel
cd /home/winterforest/Yocto/si-stm/kernel_modules/16-ioctl
make
```

用户态程序需要在 SDK 环境中编译：

```sh
. /opt/st/stm32mp15-eval-mx-gj3/4.0.4-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
make user
```

## 板上实验

加载模块：

```sh
insmod si_ioctl16.ko
ls -l /dev/si_ioctl16
```

运行用户程序：

```sh
./si_ioctl16_test
```

也可以指定设备节点：

```sh
./si_ioctl16_test /dev/si_ioctl16
```

查看内核侧每个命令的日志：

```sh
dmesg | tail -n 20
```

卸载：

```sh
rmmod si_ioctl16
```

## 命令方向

```text
_IO()       没有参数
_IOW()      用户空间 → 内核
_IOR()      内核 → 用户空间
_IOWR()     双向传递
```

本课的 `si_ioctl16_uapi.h` 同时被用户程序和内核模块包含，保证命令编号
和结构体布局一致。

`arg` 是用户空间地址，内核不能直接解引用，必须使用：

```c
copy_from_user()
copy_to_user()
```

`ioctl()` 运行在进程上下文，因此可以使用 mutex 和可能睡眠的用户空间
复制 API。成功返回 `0`，错误返回负的 errno。
