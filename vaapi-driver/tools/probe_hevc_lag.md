# HEVC 逐 AU 交付延迟探针

`probe_hevc_lag.c` 使用现有 `dmd_session` 直接访问 V4L2，绕开 VA frontend。它把带 AUD 的 Annex-B HEVC 按完整 access unit 提交，每次提交后以 200ms 超时持续取帧，直到本轮无帧。它不调用软件解码、不检查像素、不假造 READY，也不把输出顺序不同解释为时间戳错误。

## 构建与主机自测

在 `vaapi-driver/` 内：

```sh
make probe-hevc-lag
./build/probe_hevc_lag --selftest
```

工具仅依赖 C 标准库、Linux V4L2 头与 pthread，链接真实的 `dmd_v4l2_session.c`、`v4l2_backend.c`、`video_stats.c`，无需 libva/EGL/GLES。`all` 和 `tests` 不依赖此目标，也不会执行硬件探针。`--selftest` 只检查 AU 解析，覆盖三/四字节起始码、首个 AUD 之前的参数集、截取 AU 上限、缺 AUD、多图像 AU 与 EOS/EOB 拒绝。

## 生成输入与运行

用包含 HEVC B 帧的短视频生成前 16 个完整 AU，不重新编码：

```sh
ffmpeg -nostdin -i input.mkv -map 0:v:0 -frames:v 16 -an -sn -c:v copy \
  -bsf:v 'hevc_mp4toannexb,hevc_metadata=aud=insert' -f hevc sample.hevc
./build/probe_hevc_lag sample.hevc --units 16 --inspect
```

`--inspect` 仅解析，不创建硬件会话。每个 AU 必须含恰好一个 first slice；支持一图像多个 slice，不接受 EOS/EOB NAL，避免输入本身悄悄排空固件。文件上限 256MiB，单个 AU 遵守 session 的输入长度上限。

在有设备权限的容器用户下运行，推荐外层时间限制，分开保存 JSONL 与底层日志：

```sh
DMD_VA_LOG=1 DMD_V4L2_DISPLAY_DELAY=0 timeout 45s \
  ./build/probe_hevc_lag sample.hevc --units 16 --no-drain \
  > delay-off.jsonl 2> delay-off.log
DMD_VA_LOG=1 DMD_V4L2_DISPLAY_DELAY=1 timeout 45s \
  ./build/probe_hevc_lag sample.hevc --units 16 --no-drain \
  > delay-on.jsonl 2> delay-on.log
env -u DMD_V4L2_DISPLAY_DELAY DMD_VA_LOG=1 timeout 45s \
  ./build/probe_hevc_lag sample.hevc --units 16 --no-drain \
  > delay-auto.jsonl 2> delay-auto.log
```

当前 backend 的精确值 `0` 禁止 DD 请求，`1` 强制诊断请求；未设置时仅对已识别且控制项可写的现代 ABI 自动设置。旧实现按变量是否存在启用，历史测试的 off 是未设置变量，不能把旧代码的 `=0` 当作关闭。

默认配置为 HEVC、1920×1088、10bit/P010、16 AU；可用 `--width`、`--height`、`--units`（1..64）、`--timeout-ms`（1..2000）调整。尺寸应与输入及设备协商一致，探针不从 SPS 推导它们。

## 记录与判读

stdout 是 JSONL：`sent` 记录提交的 AU 号，`frame` 记录原样返回的 `unit_seq`、输出 `frame_seq`、`after_au`、几何、硬件错误与单调时间；`quiet` 表示该次超时无帧。`observation_end` 给出观测总数、`unit2_first_after_au` 和 `eos_sent:false`。unit_seq 可与该输入的已知 POC 表对应，但探针本身不解析 POC。

观测阶段绝不调用 EOS、drain 或重新创建会话。`--no-drain` 会直接销毁会话。去掉它时，仅在 `observation_end` 之后执行显式 `finish_input` 和最多约 2 秒的尾部收取，所有这些帧标记为 `phase:"teardown"`，不得计入无 EOS 观测成绩。

本轮在 SM8750P / msm_vidc 上，对同一份 Main10 B 帧样本取得的四组 A/B 及一次生产自动路径结果：

| 初始化位置 | Display Delay | 无 EOS 提交/交付 | 返回 unit_seq |
| --- | --- | --- | --- |
| 旧位置：首次 OUTPUT REQBUFS 之后 | 关闭 | 16 / 14 | 1,4,3,5,2,… |
| 旧位置：首次 OUTPUT REQBUFS 之后 | 开启 | 16 / 14 | 1,4,3,5,2,… |
| 新位置：首次 OUTPUT REQBUFS 之前 | 关闭 | 16 / 14 | 1,4,3,5,2,… |
| 新位置：首次 OUTPUT REQBUFS 之前 | 开启 | 16 / 16 | 每次提交后交付对应 1..16 |
| 新位置：首次 OUTPUT REQBUFS 之前 | 未设环境变量，自动选择现代 ABI | 16 / 16 | 每次提交后交付对应 1..16 |

新位置开启时，unit2 在提交 AU2 后已返回；生产自动路径同样在 AU2 后交付 unit2；其余三组 unit2 到提交 AU7 后才返回。这证明设置时机对该设备的真实交付行为有决定作用。控制 ioctl 返回成功本身不足以证明模式生效。自动 ABI 选择仍应在每个目标设备上用同一探针确认；该表不替代像素对拍或播放器/浏览器验收。原始输入、完整 JSONL 与设备日志由本轮对应验证记录单独归档。
