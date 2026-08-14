# Lesson 15：GPIOH11 LED 与 BSRR

PH11 接有 LED，TF-A 已将它配置为 GPIO 输出。本课不修改 `MODER`，只通过
`GPIOH_BSRR` 写入置位/清除值，控制 PH11 的输出锁存值。

## 编译

```sh
bitbake -c devshell virtual/kernel
cd /home/winterforest/Yocto/si-stm/kernel_modules/15-mmio-write-bsr
make
```

## 板上实验

加载模块：

```sh
insmod si_mmio15.ko
```

先读取 MODER 和 ODR：

```sh
cat /sys/module/si_mmio15/parameters/ph11
```

设置 PH11：

```sh
echo 1 > /sys/module/si_mmio15/parameters/ph11
```

驱动写入：

```text
寄存器：GPIOH_BSRR
地址：  0x50009018
偏移：  0x18
值：    0x00000800
作用：  设置 PH11 bit11
```

清除 PH11：

```sh
echo 0 > /sys/module/si_mmio15/parameters/ph11
```

驱动写入：

```text
寄存器：GPIOH_BSRR
地址：  0x50009018
偏移：  0x18
值：    0x08000000
作用：  清除 PH11 bit11
```

查看详细地址和值：

```sh
dmesg | tail -n 10
```

如果 `echo 1` 时 LED 熄灭、`echo 0` 时 LED 点亮，说明 LED 是低电平有效；
这是硬件连接极性，不是 BSRR 的行为变化。

卸载：

```sh
rmmod si_mmio15
```

卸载时会写入 `0x08000000` 清除 PH11 的输出锁存值。

## 寄存器地址

```text
GPIOH 基地址：0x50009000
MODER：       0x50009000
ODR：         0x50009014
BSRR：        0x50009018
PH11：        bit 11
```
