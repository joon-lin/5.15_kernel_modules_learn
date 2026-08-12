# Linux 内核 API 学习仓库

通过 Yocto devshell 学习和编译 Linux 内核模块。

## 课程目录

- `01/`：模块加载与卸载
- `02-module-param/`：模块参数
- `03-memory/`：内核内存分配与释放
- `04-char-device/`：字符设备与用户空间读写，包含 `user/si_char_test.c` 测试程序
- `05-mutex/`：互斥锁与并发访问
- `06-spinlock/`：自旋锁与短临界区
- `07-*`：后续课程

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
