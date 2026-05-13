# Hi3516 Image Processing

这个仓库用于管理 Hi3516CV610 图像处理实践项目。

`main` 分支只保留一个 `app/` 目录，表示当前版本的主线程序。历史版本不在主线里堆多个目录，而是通过 Git commit 和 tag 回看。



当前计划的稳定节点：

```text
v0.1-venc                    基础 VENC 编码链路
v0.2-venc-mp4                VENC + MP4 录像保存
v0.3-rtsp-mp4                RTSP 实时预览 + MP4 录像
v0.4-rtsp-mp4-gyro-base      陀螺仪实践主线基础版本
```

旁路验证项目建议放到独立分支：

```text
verify/rtsp                  RTSP 单独验证
verify/mpu6050-i2c           MPU6050 I2C 单独验证
exp/record-ring              预录缓冲区 / fMP4 / SD 护卡实验
exp/onvif                    ONVIF 实验
```

## 目录

```text
app/                         当前主线程序
doc/build.md                 项目和远程编译环境说明
```



说明：

本仓库主要用于源码版本管理和学习记录。完整 SDK、交叉编译链和 MPP 依赖保留在远程 Linux 编译环境。
