# Lesson 20：真实 DMAengine 传输

这个模块不只是申请 DMA 内存，而是申请 STM32 的真实 `DMA_MEMCPY` 通道，让芯片里的 MDMA 控制器把源缓冲区复制到目标缓冲区，最后由 CPU 比较两块内存验证结果。

GJ3 当前内核配置启用了 STM32 MDMA，设备树中的 `mdma1` 也处于启用状态。模块不需要外部 SPI/UART 设备或跳线。

进入 kernel devshell 后编译：

```sh
cd /home/winterforest/Yocto/si-stm/kernel_modules/20-dma
make
```

加载一次 4096 字节的 DMA 传输：

```sh
insmod si_dma20.ko
dmesg | tail -n 20
```

执行 10 次、每次 8192 字节：

```sh
insmod si_dma20.ko transfer_size=8192 transfers=10
```

卸载：

```sh
rmmod si_dma20
```

如果看到 `no DMA_MEMCPY channel`，说明当前 DMA 通道没有空闲的内存复制能力，或者 MDMA 没有成功注册；这不是普通 `dma_alloc_coherent()` 失败，而是没有可申请的 DMAengine 通道。
