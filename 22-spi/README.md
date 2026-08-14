# Lesson 22：设备树 SPI 驱动

这个课程是一个 SPI 驱动模板，包含：

- `spi_driver` 注册；
- 设备树 `compatible` 匹配；
- SPI mode 和最大频率配置；
- `spi_write_then_read()` 发送命令并读取数据；
- misc 字符设备 `/dev/spi_temp`。

## SPI 和 I2C 的区别

I2C 使用设备地址：

```text
设备地址：0x44
```

SPI 通常没有设备地址，而是使用片选线：

```text
CS0、CS1、CS2 ...
```

设备树中的：

```dts
reg = <0>;
```

表示这个 SPI 从设备使用 CS0，不表示地址 `0x00`。

## 设备树示例

把节点放到实际连接芯片的 SPI 控制器下：

```dts
&spi5 {
    status = "okay";

    lesson22@0 {
        compatible = "si,lesson22-spi-temp";
        reg = <0>;                    /* CS0 */
        spi-max-frequency = <1000000>;
        status = "okay";
    };
};
```

当前 GJ3 的 SPI5 CS0～CS4 已经有 `spidev` 节点，不能再添加相同 CS 的设备。实际使用时要选择空闲 CS，或者先移除/禁用对应的 `spidev` 节点；本课程没有直接修改板级 DTS。

## 假设的设备协议

本课假设：

```text
SPI mode       0
温度寄存器      0x00
读取命令        寄存器地址 | 0x80
返回数据        1 字节有符号摄氏度
```

代码中的关键操作：

```c
u8 command = 0x00 | BIT(7);
spi_write_then_read(spi, &command, 1, &value, 1);
```

这只是教学协议。真实芯片必须根据数据手册确认：

- SPI mode 是 0、1、2 还是 3；
- 命令字格式；
- 是否需要 dummy byte；
- 数据长度和字节序；
- 片选在多个字节之间是否必须保持有效。

## 编译和测试

进入内核 devshell 后：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/22-spi
make
```

部署设备树和模块后：

```sh
insmod si_spi22.ko
ls -l /dev/spi_temp
cat /dev/spi_temp
rmmod si_spi22
```

如果 SPI 芯片不存在，probe 或读取会失败，不会得到真实温度。
