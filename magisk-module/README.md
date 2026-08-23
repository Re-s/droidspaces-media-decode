# MediaCodec Decode Daemon — KSU/Magisk 模块（已废弃）

> ⚠️ **此方案已放弃，不再维护。**
>
> daemon 的启动、重启、守护与日志收集统一交由 **DroidSpace 平台托管**，
> 本项目不再自带进程管理。本目录保留仅供参考（例如 SELinux 规则与
> 启动时机的历史经验），**不作为发布形态**。
>
> 当前部署方式见仓库根 `README.md` 的"部署"一节。

模块的唯一职责：**开机自动启动 `decode-daemon`**。

> ⚠️ **开发和测试阶段不需要安装本模块。**
> 直接推送二进制手动启动即可，见仓库根目录 [README 的测试方法](../README.md#测试方法)。
> 模块只是最终发布形态，安装模块会引入 bootloop 风险，调试期间没必要承担。

## 当前状态

模块脚手架可用，但**尚未在真机上完整验证过安装流程**。已知待完善处见文末。

## 组成

| 文件 | 作用 |
|------|------|
| `module.prop` | 模块元信息 |
| `customize.sh` | 安装时解包与设置权限 |
| `service.sh` | `late_start service` 阶段启动 daemon |
| `sepolicy.rule` | SELinux 策略规则（见下方说明，当前多数规则冗余） |
| `META-INF/` | Magisk/KSU 安装器入口（`update-binary` 为标准空壳，由安装器接管） |

## 打包

需要先构建出 daemon 二进制并放进模块目录：

```bash
./build.sh                                  # 在仓库根目录
cp build/decode-daemon magisk-module/
cd magisk-module
zip -r ../decode-daemon-module.zip . -x '*.git*'
```

安装：Magisk Manager / KernelSU Manager → 模块 → 从本地安装 → 选择 ZIP → 重启。

## 运行时行为

- 监听端口：**20003**（与 `src/decode-daemon.c` 默认值一致）
- 日志：`/data/local/tmp/decode-daemon.log`
- PID 文件：`/data/local/tmp/decode-daemon.pid`

验证是否在运行：

```bash
adb shell "su -c 'cat /data/local/tmp/decode-daemon.log'"    # 应含 listening on 20003
adb shell "su -c 'ps -A | grep decode-daemon'"
```

## service.sh 的防 bootloop 设计

这几条不是代码风格偏好，是硬要求。KernelSU 与 Magisk 都**不对模块脚本设置超时**，
脚本阻塞会拖住开机流程。

1. **所有等待必须有上界。** 早期版本用
   `while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 1; done`
   等待开机完成，一旦该属性因任何原因永不置 1，脚本就永久阻塞。
   现在的实现带最大重试次数（120 次），超时也记录日志后继续。
2. **不用 `killall` / `pkill` 按名字杀进程。** 名字匹配存在命中系统进程的风险。
   现在只处理记录在 PID 文件里、且 `/proc/<pid>/exe` 确认指向 `decode-daemon` 的进程，
   避免 PID 复用导致误杀。
3. **停止旧实例后必须确认它真的退出。** daemon 阻塞在 `accept()` 时对 `SIGTERM`
   的响应依赖 self-pipe 唤醒；若二进制是旧版本或异常，最多等 10 秒后升级为 `SIGKILL`。
   不确认就直接启动新实例会导致两个进程同时监听同一端口
   （`SO_REUSEADDR` 会让第二次 bind 也成功），连接分配变得不可预测。
   这一点是真机实测发现的：旧版脚本会报告"已停止旧实例"但进程实际仍在运行。
4. **任何情况下都以 `exit 0` 结束**，绝不因模块自身失败影响系统启动。

## SELinux

daemon 由 KernelSU 以 root 启动，运行在 `u:r:ksu:s0` 域下，实测可正常访问
MediaCodec 与网络。**走 TCP 通道时不需要额外策略。**

> ⚠️ **但这个结论只对 TCP 成立，v0.3.0 之后不再是全貌。**
>
> 早前此处写着"不需要额外策略"且称 `sepolicy.rule` 里的 `sock_file`
> 等规则"对当前 TCP 实现无用、保留待清理"。这两句现在都要修正 ——
> v0.3.0 新增的路径式 Unix socket 通道恰恰**需要**那类权限，而且
> 当前正卡在 SELinux 上：
>
> | 启动身份 | 能 `bind()` Unix socket | 能用 MediaCodec |
> |---|---|---|
> | `u:r:ksu:s0`（本模块用的） | ✗ 各处 `EACCES` | ✓ |
> | `u:r:droidspacesd:s0` | ✓ | ✗ `CCodec::allocate` 处 SIGABRT |
>
> 也就是说 **`ksu` 域能解码但建不了 socket 文件** —— 本模块这条启动路径
> 无法支持 Unix socket 通道。需要一个兼具两种权限的 domain，
> 详见仓库根 `CHANGELOG.md` 的 v0.3.0 条目与
> [`../doc/platform-integration-contract.md`](../doc/platform-integration-contract.md) §2.2。
>
> 所以 `sepolicy.rule` 里那些 `sock_file` 规则**方向是对的**，
> 不该当作"遗留待清理" —— 只是本模块的 `ksu` 域用不上它们。

## 故障排除

**日志为空或没有 `listening`**
检查二进制是否存在且可执行：`adb shell "su -c 'ls -l /data/adb/modules/decode-daemon/'"`。
若日志出现 `bind <port> failed: Address already in use`，说明已有实例占用端口。

**容器连不上**
确认容器与 Android 共享 net namespace（DroidSpaces 默认如此），
容器内 `python3 -c "import socket;socket.create_connection(('127.0.0.1',20003),3)"` 应能连通。

**解码失败**
检查设备是否有硬件 H.264 解码器：
`adb shell "su -c 'grep -o \"OMX.qcom.video.decoder.avc\" /vendor/etc/media_codecs.xml'"`。

**模块导致无法开机**
KernelSU 的安全模式会禁用所有模块，可据此恢复；
参见 [KernelSU 救援指南](https://kernelsu.org/zh_CN/guide/rescue-from-bootloop.html)。
注意模块 `initrc/` 目录下的内容即使在安全模式下仍会执行 —— 本模块不使用 `initrc/`。

## 待完善

- 安装流程未在真机上端到端验证（开发期使用手动推送方式）
- `sepolicy.rule` 需按当前 TCP 实现精简
- 端口不可配置，需改脚本；应支持通过配置文件覆盖
