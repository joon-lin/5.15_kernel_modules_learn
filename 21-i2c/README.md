# Lesson 21：设备树匹配的 I2C 温度驱动

这是一个教学模板，假设温度芯片：

- I2C 地址为 `0x44`；
- 温度寄存器为 `0x00`；
- 寄存器返回一个有符号 8 位摄氏度值。

真实芯片的命令格式必须按照数据手册修改。特别是 SHT31 虽然常用地址也是 `0x44`，但它不是本课假设的“寄存器读一个字节”协议。

## 设备树

把下面节点放到实际连接该温度芯片的 I2C 控制器下，例如 `&i2c2`：

```dts
&i2c2 {
    status = "okay";

    temp@44 {
        compatible = "si,lesson21-temp";
        reg = <0x44>;
        status = "okay";
    };
};
```

这里：

- `reg = <0x44>` 提供 I2C 从设备地址；
- `compatible` 匹配驱动的 `of_match_table`；
- 匹配成功后调用 `si_temp21_probe()`。

如果 0x44 上没有真实芯片，probe 会失败，系统不会创建 `/dev/temp`。本模板没有直接修改 GJ3 的实际 DTS，避免给没有接入的地址增加无效设备节点。

## 两种字符设备写法

`make` 会同时编译两个模块：

```text
si_temp21.ko       misc 写法，设备节点 /dev/temp
si_temp21_cdev.ko  手动 cdev 写法，设备节点 /dev/temp_cdev
```

两种驱动使用不同的 `compatible`，用于对比学习。它们不能同时绑定同一个设备树节点。

手动 cdev 版本的注册过程是：

```text
alloc_chrdev_region()
    -> cdev_init()/cdev_add()
    -> class_create()
    -> device_create()
    -> /dev/temp_cdev
```

它的 `open()` 中会自己完成：

```c
data = container_of(inode->i_cdev,
                   struct si_temp21_cdev_data, cdev);
file->private_data = data;
```

这正是它和 `misc` 版本的关键区别：`misc` 框架会自动设置 `file->private_data`，而普通 `cdev` 需要驱动自己设置。

如果要测试 cdev 版本，把设备树节点的 `compatible` 改为：

```dts
temp@44 {
    compatible = "si,lesson21-temp-cdev";
    reg = <0x44>;
    status = "okay";
};
```

然后使用：

```sh
insmod si_temp21_cdev.ko
cat /dev/temp_cdev
rmmod si_temp21_cdev
```

## 编译和测试

先进入内核 devshell：

```sh
bitbake virtual/kernel
bitbake -c devshell virtual/kernel
```

然后编译：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/21-i2c
make
```

把模块和编译后的设备树部署到开发板后：

```sh
insmod si_temp21.ko
ls -l /dev/temp
cat /dev/temp
rmmod si_temp21
```

正常时，`cat /dev/temp` 的调用链是：

```text
cat /dev/temp
  -> si_temp21_read()
  -> i2c_smbus_read_byte_data()
  -> I2C 地址 0x44、寄存器 0x00
```
