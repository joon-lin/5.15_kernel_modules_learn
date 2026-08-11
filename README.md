# Linux 内核 API 学习仓库

通过 Yocto devshell 学习和编译 Linux 内核模块。

## 课程目录

- `01/`：模块加载与卸载
- `02-module-param/`：模块参数
- `03-*`：后续课程

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
