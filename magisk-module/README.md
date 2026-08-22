# MediaCodec Decode Daemon - Magisk Module

## 概述
这是一个Magisk模块，用于在Android设备上运行MediaCodec硬件解码daemon，为DroidSpaces容器提供视频解码服务。

## 功能特性
- 开机自启decode-daemon服务
- 自动配置SELinux策略
- 支持H.264和HEVC硬件解码
- 使用Unix socket与容器客户端通信
- 零拷贝帧传递（通过文件描述符）

## 安装要求
- 已安装Magisk 20.0+
- Android 10.0+ (API 29+)
- 支持MediaCodec硬件解码的设备
- ARM64架构

## 安装步骤
1. 下载模块ZIP文件
2. 打开Magisk Manager应用
3. 点击"模块" → "从本地安装"
4. 选择下载的ZIP文件
5. 等待安装完成，重启设备

## 配置说明
### Socket路径
默认socket路径：`/tmp/anland/decode.sock`
可通过修改`service.sh`中的`SOCKET_PATH`变量更改。

### SELinux策略
模块会自动配置SELinux策略，允许decode-daemon：
- 创建和使用Unix socket
- 访问硬件设备文件
- 与surfaceflinger等系统服务交互

### 日志文件
daemon日志保存在：`/data/local/tmp/decode-daemon.log`

## 使用方法
### 容器客户端连接
```c
// C语言示例
int sock = socket(AF_UNIX, SOCK_STREAM, 0);
struct sockaddr_un addr;
addr.sun_family = AF_UNIX;
strcpy(addr.sun_path, "/tmp/anland/decode.sock");
connect(sock, (struct sockaddr*)&addr, sizeof(addr));

// 发送NALU数据
uint32_t size = nalu_size;
send(sock, &size, 4, 0);
send(sock, nalu_data, size, 0);

// 接收解码帧
uint32_t width, height, stride;
recv(sock, &width, 4, 0);
recv(sock, &height, 4, 0);
recv(sock, &stride, 4, 0);
// 接收文件描述符用于零拷贝访问
```

### Python客户端示例
```python
import socket
import struct

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect('/tmp/anland/decode.sock')

# 发送NALU
size = len(nalu_data)
sock.send(struct.pack('I', size))
sock.send(nalu_data)

# 接收帧信息
width = struct.unpack('I', sock.recv(4))[0]
height = struct.unpack('I', sock.recv(4))[0]
stride = struct.unpack('I', sock.recv(4))[0]
```

## 故障排除
### 1. Daemon无法启动
- 检查日志文件：`cat /data/local/tmp/decode-daemon.log`
- 确认Magisk模块已激活
- 重启设备

### 2. SELinux权限问题
- 检查SELinux状态：`getenforce`
- 临时设为宽容模式测试：`setenforce 0`
- 检查SELinux日志：`dmesg | grep avc`

### 3. Socket连接失败
- 确认socket文件存在：`ls -la /tmp/anland/decode.sock`
- 检查文件权限：`chmod 666 /tmp/anland/decode.sock`
- 确认容器有访问权限

### 4. 解码失败
- 检查MediaCodec支持：`dumpsys media.codec`
- 确认NALU格式正确
- 检查帧大小限制（最大8MB）

## 卸载方法
1. 打开Magisk Manager应用
2. 点击"模块"
3. 找到"MediaCodec Decode Daemon"
4. 点击"卸载" → 重启设备

## 技术规格
- **协议格式**：
  - 输入：`[4B size][NALU data]`
  - 输出：`[4B width][4B height][4B stride][1B format][fd][frame data]`
- **支持编解码器**：H.264 (video/avc), HEVC (video/hevc)
- **最大帧大小**：8MB
- **超时时间**：5秒

## 开发信息
- 基于Android NDK MediaCodec API
- 使用Unix domain socket通信
- 支持文件描述符传递实现零拷贝
- 自动检测H.264/HEVC格式

## 许可证
本模块为开源项目，遵循Apache 2.0许可证。