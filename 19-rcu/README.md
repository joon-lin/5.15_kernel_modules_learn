# Lesson 19：RCU

本课演示读多写少场景下的 RCU：一个内核线程持续读取共享对象，延迟工作周期性创建新对象并发布，旧对象通过 `call_rcu()` 在宽限期后回收。

进入 kernel devshell 后编译：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/19-rcu
make
```

加载并观察日志：

```sh
insmod si_rcu19.ko
dmesg -w
```

参数示例：

```sh
# 每 100 ms 更新一次，每 200 ms 读取一次，只更新 5 次
insmod si_rcu19.ko update_interval_ms=100 reader_interval_ms=200 updates=5
```

卸载：

```sh
rmmod si_rcu19
```

重点观察：对象发布后不会立即释放，`call_rcu()` 要等已有读者离开 `rcu_read_lock()` 之后才执行回收回调。
