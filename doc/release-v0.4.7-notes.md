# v0.4.7 发布说明

设备：骁龙 8 Elite / Adreno 830，内核 6.6.118-android15，DroidSpaces 容器，
Chrome 151.0.7922.169，libva 2.23（libva 1.23），Wayland 宿主。
产物版本串形如 `DroidSpaces V4L2 VA-API driver 0.4.7+<8 位 git 短 hash>` ——
构建号随提交变化，装完以 `vainfo` 打印的那一行为准核对；
产物本身的唯一标识是 GitHub Release 上附带的 `.so` 及其 sha256。

## 这一版解决什么

**浏览器里偶发的 GPU 进程崩溃（`Crashing due to FD ownership violation` /
`GPU process exited unexpectedly: exit_code=5`）—— 两个独立成因都修了。**

成因一（`5b415fc`）：IO 所有权标记 `io_busy[]` 提前交还。收帧线程还攥着会话
指针和帧缓冲时，`DestroyContext` 就把会话拆了、`close()` 掉 fd，宿主随后把
同号 fd 复用成别的文件，下一次 QBUF 打到别人的 fd 上。改法是把不变量贯彻到
全部 12 处 IO：`io_busy` 必须覆盖**最后一次**使用，会话销毁挂进 `io_defer[]`
由最后一个使用者执行，`c->retiring` 阻止收帧线程复活将死的会话。

成因二（`d92e429`，真凶）：`dmd_v4l2_open()` 里 `memset(d, 0, ...)` 之后只把
`out[]`/`cap[]` 的 `dbuf_fd` 置 -1，**漏了 `extra[]`**；而 `dmd_v4l2_close()`
无条件 `bufs_free(d->extra, 24)`，判据是 `dbuf_fd >= 0` —— 于是每拆一次会话就
对 **fd 0** 调 24 次 `close()`。Chromium 拦截 `close()` 并按 `ScopedFD` 检查归属
（`base/files/scoped_file_linux.cc`：`IsFDOwned(fd)` → `CrashOnFdOwnershipViolation()`），
fd 0 一旦被 Chrome 自己占用，我们下一次拆会话就当次打死 GPU 进程。
"时有时无"完全取决于 fd 0 此刻在谁手里，与时序竞态无关。

机制级判据（不靠"这次没崩"）：`strace -f -e trace=close`，同一条 1280x720 AV1，
修复前 `close(0)` **24 次**（第 1 次返回 0，真的关掉了宿主进程的 stdin，其余
`EBADF`），修复后 **0 次**。

ffmpeg 路径永远看不出这个 bug —— 它不记账 fd 归属，rc 仍是 0、像素仍逐字节一致。

## 实测数据

崩溃 A/B（每 400 ms 换一次解码源，专打"边收帧边拆会话"）：

| 驱动 | 结果 |
|---|---|
| `0.4.7+232600f8` | 启动约 **26 秒**后 `exit_code=5` |
| `0.4.7+5b415fc8` | 约 17 分钟内崩 **3 次**（900 ms 强度下干净） |
| `0.4.7+d92e4299` | 548 秒：**1352 个上下文 / 1351 场正常收尾 / 崩溃 0 / ENOTTY 0 / 等 IO 所有权超时 0** |

浏览器侧逐条验证（同一台机、同一个驱动）：

| 项 | 结果 |
|---|---|
| AV1 480p / 720p / 1080p 换分辨率冲刷 | 60 场会话，送入 == 收到，逐场平衡 |
| AV1 **4K**（3840x2160，level 12） | 60 上下文，送入 9827 / 收到 9827，不平衡 0，崩溃 0 |
| **三路并发**（480p+720p+1080p 同时播 + 随机 seek） | 3 上下文并存，32 次 seek，卡住 0、错误 0，驱动写入 3645 帧 |
| **MSE 路径**（自建测试台：逐段 append、中途回退 seek 不进关键帧、`changeType` 带内换分辨率） | 11 上下文，送入 647 / 收到 647，切换 1 次 0 失败，页面 dropped frames 0 |
| 像素正确性（ffmpeg 契约 + Chrome 契约两趟） | AV1 480p/720p/1080p 与软解**逐字节一致**；H.264 逐字节一致 |

## 已知限制（不是回归，是尚未实现）

**AV1 10-bit（P010）在浏览器里走不了硬解。** 硬件与驱动本身没问题：真 10-bit
流走 ffmpeg 路径输出 `p010le` 与软解逐字节一致。卡点在导出 —— Chrome 与
Firefox 传的都是 `flags=0x5`（`SEPARATE_LAYERS | READ_ONLY`），而驱动对 10-bit
的分离层明确返回不支持（本机 libdrm 没有能准确表达 P010 交织 UV 的 DRM 四cc，
借用 16bit 原生格式会让消费方按 16bit 量程采样、整幅暗 64 倍）。日志指纹：

```
ExportSurfaceHandle: surface 1 是 P010，SEPARATE_LAYERS 未实现（flags=0x5）
vaapi_wrapper.cc:2756] vaExportSurfaceHandle failed, VA error: the requested RT Format is not supported
```

表现是会话建了、`送入 0`，然后浏览器静默走软解 —— 看着像"配了没生效"。
下一步补 10-bit 分离层格式。

**解码器节点写死 `/dev/video32`**：节点号不同的机器建不了会话。装前先
`ls -l /dev/video32` 确认。

**本机（SM8150/nabu）固件不支持 AV1**，发布版驱动默认也不声明该 profile；
B 站等站点默认给 AV1 时会静默软解。别的机型以 `vainfo` 与编码表为准。

## 文档纠错（重要，会直接影响能不能用）

1. **`--enable-features=Vulkan` 任何机型都不能加**（等同 `chrome://flags` 里那个
   "Vulkan"）。加上它 GPU 进程仍会探测、仍建满 config，然后直接 `vaTerminate`，
   **一个解码上下文都不建**（实测矩阵：`CreateContext` 1/0/1/0，唯一变量就是这个
   feature）。显示需要的是 `--use-angle=vulkan`，它只切 ANGLE 后端、实测不影响
   硬解，8 Elite 不给它会文字糊加重影。nabu / SD855 那代则要把它去掉。
2. **命令行只能打开 Vulkan，关不掉**：Chrome 151 二进制里没有 `--disable-vulkan`，
   `--disable-features=Vulkan`、`--use-vulkan=disabled` 实测全部无效（既不报错也不
   生效）。要关只能进 `chrome://flags`，换机器/重建 profile 后得再改一次。
3. **`DMD_VA_LUMA` 那个黑帧/绿帧计数只在 Firefox 路径有效**。Chrome 是在解码之前
   就导出 surface，此刻像素注定全零 —— 一次 MSE 压测 162 次采样全报"黑帧"，而帧
   本身是正常解出来的。别拿它在 Chrome 上判画质。

## 安装

Chrome 参数一次性配好（幂等，重复执行不叠加）：

```bash
bash tools/configure-chrome-vaapi.sh
```

或手工给 `.desktop` 注入：

```
--ozone-platform=wayland --render-node-override=/dev/dri/renderD128
--ignore-gpu-blocklist --use-angle=vulkan
--enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL
```

完整原理、Firefox 侧的四个 pref、以及逐条排查见
[`doc/browser-vaapi-guide.md`](browser-vaapi-guide.md)。

## 还没测过的

4K 只测了单源冲刷（多路 4K、4K 长时间未测）；真实站点（B 站登录态下的
高码率档位、DRM 内容）本轮未跑；HEVC/VP9 未重跑浏览器压测；
`/dev/video32` 之外的机型未验证。
