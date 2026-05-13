# 项目与编译环境说明

这个仓库主要用于项目管理、版本记录和源码归档。

拉到本机后不要求直接编译。真正的 SDK、MPP 公共 Makefile、交叉编译链和 RTSP 依赖都在远程 Linux 编译环境：

```text
192.168.1.10
```

## 远程编译环境

```sh
ARCH=arm
CROSS_COMPILE=arm-v01c02-linux-musleabi-
```

工程依赖海思 SDK 的 sample 公共构建文件，例如：

```text
Makefile.param
$(ARM_ARCH)_$(OSTYPE).mak
```

这些文件不放进 GitHub 仓库，保持在远程 Linux 的 SDK 目录里。

## 主线程序

```text
app/
```

`app/` 是当前主线。历史阶段不在 main 中堆目录，使用 Git commit/tag 回看。

如果需要在远程 Linux 上编译，把 `app/` 同步到 SDK 的 `mpp/sample` 目录下，再执行：

```sh
cd app
make clean
make
```

生成目标通常为：

```text
sample_venc
```

## Git 管理约定

`main` 只放当前主线程序。

历史稳定节点使用 tag：

```text
v0.1-venc
v0.2-venc-mp4
v0.3-rtsp-mp4
v0.4-rtsp-mp4-gyro-base
```

实验和模块验证使用分支：

```text
verify/mpu6050-i2c
verify/rtsp
exp/record-ring
exp/onvif
```
