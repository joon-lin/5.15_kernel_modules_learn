# Lesson 13：sysfs

这一课创建一个简单的 sysfs 接口：

```text
/sys/class/si_lesson13/demo/message
/sys/class/si_lesson13/demo/write_count
```

## 编译

进入 Yocto 内核 devshell：

```sh
bitbake -c devshell virtual/kernel
```

然后编译：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/13-sysfs
make
```

## 实验

加载模块：

```sh
insmod si_sysfs13.ko
```

查看属性：

```sh
ls -l /sys/class/si_lesson13/demo
cat /sys/class/si_lesson13/demo/message
cat /sys/class/si_lesson13/demo/write_count
```

写入属性：

```sh
echo "hello kernel" > /sys/class/si_lesson13/demo/message
cat /sys/class/si_lesson13/demo/message
cat /sys/class/si_lesson13/demo/write_count
```

卸载模块：

```sh
rmmod si_sysfs13
```

## 本课 API

- `class_create()`：创建 `/sys/class/si_lesson13/`；
- `device_create()`：创建 `demo` 设备目录；
- `DEVICE_ATTR_RW()`、`DEVICE_ATTR_RO()`：定义属性；
- `sysfs_create_group()`：创建一组属性文件；
- `sysfs_emit()`：向 sysfs 读取缓冲区格式化输出；
- `device_destroy()`、`class_destroy()`：卸载时清理对象。

sysfs 的 `show()` 和 `store()` 回调收到的是内核缓冲区，因此不像字符设备
的 `read()`/`write()` 那样需要 `copy_to_user()` 或 `copy_from_user()`。
