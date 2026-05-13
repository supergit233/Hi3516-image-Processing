# RTSP + MP4 ring recording

这一版在 `venc_rtsp_mp4` 里实现：

```text
H.264 通道 -> RTSP 实时播放
H.265 通道 -> MP4 分段环形录像
```

默认只录 H.265，避免 H.264 实时流再额外占一份存储。需要时也可以通过环境变量改成录 H.264 或两个都录。

## 编译

在 SDK sample 目录下进入 `venc_rtsp_mp4`：

```sh
make clean
make
```

生成的程序还是：

```text
sample_venc
```

## 运行配置

默认配置：

```text
录像目录        /home/data
单段时长        30 秒
环形段数        120 段
录像编码        h265
```

程序会按这个顺序读取配置：

```text
1. 代码内默认值
2. 优先读取 ./record.conf；如果没有，再读 /etc/sample_record.conf
3. 环境变量覆盖
```

也可以显式指定配置文件：

```sh
export SAMPLE_RECORD_CONF=/etc/sample_record.conf
./sample_venc
```

配置文件格式：

```conf
RECORD_DIR=/home/data
SEGMENT_SEC=30
MAX_SEGMENTS=120
CODEC=h265
```

运行前临时覆盖：

```sh
export SAMPLE_RECORD_DIR=/mnt/video
export SAMPLE_RECORD_SEGMENT_SEC=30
export SAMPLE_RECORD_MAX_SEGMENTS=120
export SAMPLE_RECORD_CODEC=h265
./sample_venc
```

`SAMPLE_RECORD_CODEC` 支持：

```text
h265    只录 H.265，默认
h264    只录 H.264
all     H.264/H.265 都录
```

## 文件形式

录像文件按固定槽位覆盖：

```text
/mnt/video/chn0_000.mp4
/mnt/video/chn0_001.mp4
/mnt/video/chn0_002.mp4
...
```

槽位数量由 `SAMPLE_RECORD_MAX_SEGMENTS` 控制。比如：

```text
SAMPLE_RECORD_SEGMENT_SEC=30
SAMPLE_RECORD_MAX_SEGMENTS=120
```

约等于保留最近 1 小时录像：

```text
30 秒 * 120 段 = 3600 秒
```

## 切段策略

程序不是到时间就硬切，而是：

```text
达到目标帧数后，等待下一个关键帧，再关闭旧 MP4，打开新 MP4
```

这样新段尽量从 IDR/I 帧开始，避免 MP4 从 P 帧开始导致播放器打不开。

## 注意

MP4 的 `moov` 索引在关闭文件时写入，所以异常断电时当前正在写的段可能损坏，之前已经关闭的段一般可播放。

如果录像目录不存在，程序会尝试 `mkdir`。如果使用 `/mnt/video`，建议先确认存储已经挂载：

```sh
mount
df -h
ls /mnt/video
```
