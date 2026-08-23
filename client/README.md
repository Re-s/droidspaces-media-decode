# decode-client — 容器侧解码客户端

运行在 Linux 容器内：解复用视频文件 → 把 NALU 送到 Android 侧的 `decode-daemon`
→ 取回解码后的 NV12 帧 → 用 EGL/GLESv2 渲染到 X11 窗口，或导出为 PPM。

## 构建

依赖（Debian/Ubuntu）：

```bash
apt-get install -y build-essential cmake pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev \
    libegl1-mesa-dev libgles2-mesa-dev libx11-dev
```

构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物 `build/decode-client`。

## 使用

先确保 Android 侧 daemon 已在运行（见仓库根 [README](../README.md#测试方法)）。

```bash
# 最常用：TCP 连接 daemon，导出 PPM 不开窗口
./build/decode-client -s 20003 --no-display -o out/ test.h264

# 渲染到 X11 窗口
./build/decode-client -s 20003 video.mp4

# 循环播放，最多 400 帧
./build/decode-client -s 20003 -l -n 400 test.h264

# 从标准输入读裸流
cat test.h264 | ./build/decode-client -s 20003 -
```

`-s` 参数的解析规则：

| 形式 | 含义 |
|------|------|
| 纯数字，如 `20003` | TCP `127.0.0.1:<端口>`，当前 daemon 使用这种 |
| 路径，如 `/tmp/x.sock` | Unix domain socket |
| `@` 开头，如 `@/anland/decode.sock` | abstract namespace socket |

## 实测结果

| 流 | 送出单元 | 解码帧 | 缓冲尺寸 | 导出 PPM |
|----|---------|--------|----------|----------|
| 1080p H.264 | 163 NALU | 150 | 1920×1088 | 1920×1080（6220817 字节） |
| 720p HEVC | 170 NALU | 150 | 1280×720 | 1280×720 |
| 720p VP9 | 120 帧 | 120 | 1280×720 | 1280×720 |
| 720p VP8 | 120 帧 | 120 | 1280×720 | 1280×720 |

H.264 的 PPM 按显示裁剪区域导出，比缓冲尺寸少 8 行 —— 那 8 行是解码器的对齐填充。
VP8/VP9 的输入帧数与输出帧数精确相等（无 B 帧重排序）。
循环模式 3 轮 350 帧，约 194 fps。

## 实现要点

以下几条都是实测踩出来的，改动相关代码前请先读：

**必须交错收发。** 若客户端"先发完全部 NALU 再开始接收"，daemon 的写会填满
socket 缓冲并阻塞 —— 双方僵死，解码帧全部丢失（实测 0 帧）。
改为边发边收后同一码流稳定取回 148 帧。

> 这条约束**至今仍然成立**，但原因已经变了。当初是因为 daemon 单线程串行
> （每收一个 NALU 就尝试取一帧并写回同一 socket）。现在 daemon 已改成
> 收发分离的多线程结构（`decode-daemon.c` 有 3 处 `pthread_create`），
> 可是**约束依然存在** —— 只要客户端不读，socket 缓冲总会填满，
> 写端最终阻塞。这是 TCP 流控的固有性质，与 daemon 内部结构无关。
>
> 换句话说：拆线程解决的是**吞吐**（单客户端提升 20–30%、支持多客户端并发），
> 不解决"客户端必须边发边收"这个协议层要求。

**NALU 必须带 Annex B start code。** demuxer 产出的 NALU 不含起始码，
`comm_send_nalu` 会统一补上 4 字节 `00 00 00 01`。
缺起始码时 daemon 无法定位 `nal_unit_header`，MediaCodec 也无法解析码流，
表现为全部入队但输出 0 帧。

**SPS/PPS 在 extradata 里，不在 packet 里。** FFmpeg 把参数集放在
`AVCodecParameters::extradata`（裸 H.264 流为 Annex B，MP4 为 avcC/hvcC）。
`demuxer.c` 会在第一个 NALU 之前主动送出它们；不送则解码器拿不到序列参数，
同样是"全部入队、零输出"。

**缓冲尺寸 ≠ 显示尺寸。** 1080p 输入返回的缓冲是 1920×**1088**，
末 8 行是对齐填充（内容为最后一行的复制）。客户端连接时会握手，
拿回 `stride` / `slice_height` / 显示裁剪区域：

- UV 平面起点是 `stride × slice_height`，**不是** `width × height`
- 渲染与导出按 crop 裁剪，所以 PPM 是 1920×1080
- 逐行读取必须用 `stride` 当行距，用显示宽度会让画面逐行错位（斜切）

握手是必需的，失败会直接中止 —— 客户端与 daemon 配套发布，没有兼容模式。

**共享内存传输（`--shm`）。** 帧数据放在 memfd 里，socket 只传槽位号，
省掉 TCP 的两次内核拷贝。`DecodedFrame.data` 直接指向共享内存，
客户端侧也没有额外拷贝，因此 `data_alloc` 为 0（不是自有内存，不能 free）。
每处理完一帧必须调 `comm_release_frame` 归还槽位，否则 daemon 会判定客户端卡死。
实测 1080p 吞吐 166.8 → 194.7 fps，daemon CPU 降 28.6%，
两种模式导出的 PPM 逐字节一致。daemon 可能降级为 TCP，用 `comm_get_xfer` 查实际模式。

**流内分辨率变化会重新下发格式块。** daemon 已启用 adaptive-playback，
分辨率变化时插入一个哨兵帧头（`frame_size == 0xFFFFFFFF`）引出新格式。
`comm_recv_frame` 内部处理这个哨兵，每帧后重新调 `comm_get_format` 即可拿到最新值。
实测 720p→480p 拼接流可正确切分成 60 帧 1280×720 + 60 帧 640×480。

**多编解码器。** demuxer 识别流类型后，握手时声明对应的 codec id，
同一个二进制能解 H.264 / HEVC / VP9 / VP8，无需切换参数。

**两类码流的切分方式不同。** H.264/HEVC 按 Annex B 起始码切成 NALU，
参数集从 extradata 注入；VP8/VP9 没有 NALU 概念，一个 packet 就是一整帧，
走"整帧模式"直接送出，且**必须关闭起始码补齐**（`comm_set_annexb(comm, 0)`）——
给 VP8/VP9 补 4 字节起始码会破坏帧数据，解码器拒绝整个码流。

**循环播放需要重连。** daemon 每个连接只配置一次解码器，
且在客户端关闭写端后进入 flush 并结束会话。
单纯 seek 回起点并重发 SPS/PPS 不会让它重建解码器，因此 `-l` 每轮都新建连接。
裸码流没有索引，`av_seek_frame` 会失败，`demuxer_seek_start` 会回退到 `avio_seek`。

## 已知限制

- 帧数据经历完整 CPU 拷贝，未使用零拷贝路径（可行性已验证，见
  [性能与路线](../doc/performance-and-roadmap.md)）
- `-f <fps>` 帧率限制参数已接受但未实现节流
- 容器内 EGL 只有 `EGL_PLATFORM_DEVICE_EXT` 能拿到 `zink over turnip`；
  X11 / GBM / Wayland 平台的 `eglInitialize` 均失败（`ZINK: failed to choose pdev`），
  surfaceless 会退回 `llvmpipe` 软件光栅。**这意味着 X11 渲染路径当前可能跑在软件光栅上**，
  尚未独立排查。用 `--no-display -o` 导出 PPM 不受影响。
