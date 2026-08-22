# DroidSpaces Media Decode Daemon

Android MediaCodec 硬件解码代理服务，为 Linux 容器提供视频硬解能力。

## 项目简介

本项目提供了一个运行在 Android 设备上的 MediaCodec 硬件解码守护进程，通过 TCP socket 将硬件解码能力暴露给 Linux 容器（如 DroidSpaces 中的 Debian 容器）。这使得容器内的应用程序能够利用 Android 设备的 GPU 硬件解码能力，实现高效的视频播放。

### 核心特性

- **硬件加速解码**：利用 Android MediaCodec API 进行 H.264 硬件解码
- **TCP 跨 namespace 通信**：通过 TCP socket 绕过 SELinux 限制，实现 Android 与 Linux 容器间的通信
- **最小化实现**：基于 anland 项目的 libdisplay_daemon 库简化而来，代码简洁易懂
- **KSU/Magisk 模块**：提供开箱即用的 Magisk 模块，支持开机自启和 SELinux 策略配置

## 架构图

```
┌─────────────────────────────────────────────────────┐
│                Android 设备 (Root)                  │
│                                                     │
│  ┌───────────────────────────────────────────────┐ │
│  │         decode-daemon (TCP 服务端)            │ │
│  │                                               │ │
│  │  • 监听 TCP 端口 (默认 20003)                │ │
│  │  • 接收 H.264 NALU 数据                      │ │
│  │  • 使用 MediaCodec 硬件解码                   │ │
│  │  • 返回解码后的 NV12 帧数据                  │ │
│  └───────────────────────┬───────────────────────┘ │
│                          │ TCP (127.0.0.1:20003)    │
│                          │                          │
│  ┌───────────────────────┴───────────────────────┐ │
│  │          Linux 容器 (Debian aarch64)          │ │
│  │                                               │ │
│  │  • 客户端程序 (test_decode.py)                │ │
│  │  • 连接 TCP 服务端                            │ │
│  │  • 发送 H.264 NALU 流                        │ │
│  │  • 接收解码帧用于显示或处理                   │ │
│  └───────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

### 通信协议

- **输入格式**：`[4字节 NALU 长度 (大端)][NALU 数据]`
- **输出格式**：`[4字节 宽度][4字节 高度][4字节 帧大小][帧数据]`
- **最大帧大小**：8MB
- **超时时间**：5 秒

## 编译方法

### 环境要求

- Android NDK r27c 或更高版本
- 支持 ARM64 架构的编译环境
- Android API Level 21+ (MediaCodec NDK 支持)

### 编译步骤

1. **设置 NDK 环境**
   ```bash
   # 下载 Android NDK r27c
   # https://developer.android.com/ndk/downloads
   
   export NDK=/path/to/android-ndk-r27c
   export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
   export TARGET=aarch64-linux-android
   export API=21
   ```

2. **编译 decode-daemon**
   ```bash
   cd src
   
   # 交叉编译
   $TOOLCHAIN/bin/${TARGET}${API}-clang \
       -O2 -Wall -Wextra \
       -o decode-daemon \
       decode-daemon.c \
       -lmEDIAndk -lmediandk -llog -landroid
   ```

3. **编译选项说明**
   - `-lmEDIAndk`：MediaCodec NDK 库
   - `-lmediandk`：MediaFormat NDK 库
   - `-llog`：Android 日志库
   - `-landroid`：Android 基础库

### 快速编译脚本

创建 `build.sh` 脚本：
```bash
#!/bin/bash
NDK=${NDK:-/opt/android-ndk-r27c}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
TARGET=aarch64-linux-android
API=21

$TOOLCHAIN/bin/${TARGET}${API}-clang \
    -O2 -Wall -Wextra \
    -o decode-daemon \
    decode-daemon.c \
    -lmEDIAndk -lmediandk -llog -landroid

echo "编译完成: decode-daemon"
```

## KSU/Magisk 模块安装方法

### 安装要求

- 已安装 Magisk 20.0+ 或 KernelSU
- Android 10.0+ (API 29+)
- 支持 MediaCodec 硬件解码的设备
- ARM64 架构

### 安装步骤

1. **准备模块包**
   ```bash
   # 进入 magisk-module 目录
   cd magisk-module
   
   # 确保 decode-daemon 二进制文件已编译并放置在此目录
   # 可以从 src/ 目录复制编译好的文件
   cp ../src/decode-daemon .
   
   # 打包为 ZIP 文件
   zip -r ../decode-daemon-module.zip . -x "*.git*"
   ```

2. **安装模块**
   - 打开 Magisk Manager 应用
   - 点击"模块" → "从本地安装"
   - 选择生成的 `decode-daemon-module.zip` 文件
   - 等待安装完成，重启设备

3. **验证安装**
   ```bash
   # 检查模块是否激活
   magisk --list
   
   # 检查服务是否运行
   ps -ef | grep decode-daemon
   ```

### 模块配置

- **Socket 路径**：默认 `/tmp/anland/decode.sock`，可在 `service.sh` 中修改
- **日志文件**：`/data/local/tmp/decode-daemon.log`
- **SELinux 策略**：模块自动配置，允许 daemon 访问硬件设备和创建 socket

## 测试方法

### 1. 准备测试环境

确保 Android 设备已 root，并已安装 Magisk 模块。

### 2. 准备测试视频

在 Android 设备上准备一个 H.264 测试文件：
```bash
# 使用 adb 推送测试文件
adb push test_video.h264 /data/local/tmp/
```

### 3. 运行测试脚本

在 Linux 容器内运行 Python 测试脚本：
```bash
# 连接 TCP 服务端并发送 NALU
python3 test_decode.py 20003 /data/local/tmp/test_video.h264
```

### 4. 预期输出

```
Connected to port 20003
Sent 30 NALUs
Frame 1: 1920x1080 3110400 bytes
Frame 2: 1920x1080 3110400 bytes
...
RESULT: 30 frames decoded from /data/local/tmp/test_video.h264
```

### 5. 手动测试

```bash
# 在 Android 设备上启动 daemon（如果未使用 Magisk 模块）
adb shell
su
./decode-daemon 20003

# 在容器内连接测试
nc -zv 127.0.0.1 20003
```

## 已知问题

### 1. 分辨率限制
- 当前代码硬编码为 1920x1080 分辨率
- 解码后的帧格式固定为 NV12
- 需要修改源码以支持动态分辨率

### 2. 仅支持 H.264
- 目前仅支持 H.264 (video/avc) 解码
- 未实现 HEVC、VP9 等其他编解码器支持
- 需要扩展 AMediaFormat 配置

### 3. 单客户端连接
- 每次只支持一个客户端连接
- 新连接会中断当前会话
- 需要实现多客户端支持和会话管理

### 4. TCP 通信安全性
- 使用明文 TCP 通信，无加密
- 仅绑定 loopback 地址 (127.0.0.1)
- 不适用于跨网络传输

### 5. 错误处理不完善
- 部分错误情况未正确处理
- 可能存在内存泄漏
- 需要增加更健壮的错误恢复机制

### 6. 编解码器参数固定
- 编解码器参数（如 profile、level）未动态协商
- 可能不兼容某些特殊编码的视频流

## 开发信息

- **源码基于**：anland 项目的 libdisplay_daemon 库
- **技术栈**：Android NDK (C), Python 测试脚本
- **通信方式**：TCP socket (IPv4 loopback)
- **目标平台**：Android ARM64 设备

## 相关文档

- [VAAPI Proxy 架构调研报告](doc/vaapi-mediacodec-proxy-research.md) - 详细的 VA-API 代理驱动实现方案
- [Magisk 模块详细文档](magisk-module/README.md) - 模块安装和配置说明

## 许可证

本项目遵循 Apache 2.0 许可证。

## 贡献

欢迎提交 Issue 和 Pull Request。在贡献代码前，请确保：
1. 代码符合项目风格
2. 添加必要的注释
3. 更新相关文档
4. 测试通过