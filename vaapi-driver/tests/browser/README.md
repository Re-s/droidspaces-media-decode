# 浏览器侧帧序验证

用于验证 Chrome 实际上屏的帧顺序与画面内容，覆盖 ffmpeg 测不到的路径。

## 为什么需要浏览器测试

`tests/regress_chrome_contract.sh` 用 `DMD_NO_MAP_WAIT=1` 模拟 Chrome 的
"EndPicture 返回即采样"契约，但它仍然是 ffmpeg 在驱动。有两件事只有真实
浏览器能覆盖：

- **MSE 分段喂流**：在线视频走 `appendBuffer`，时间轴基准与直读 `src` 不同
- **实际上屏内容**：Chrome 的软件 DPB 决定哪个 surface 何时上屏

## 判据

`requestVideoFrameCallback` 给出每次上屏的 `mediaTime`，据此算出帧号；
同时用 canvas 抓像素算签名，与软解基线比对，得到"画面里实际是第几帧"。
两者不符 = 时间戳对但画面错，那才是"前后帧跳跃"。

⚠️ **`rvfc` 会漏采**，单看帧号单调性判据不足。实测：旧版驱动
（`9f00b1a6`，无收帧线程）与修复版跑出的帧号序列逐值相同，都单调递增 ——
说明只看单调性测不出问题，必须做像素比对。

⚠️ **fMP4 有时间轴偏移**。`-movflags frag_keyframe+empty_moov` 生成的分段流
`start_time=1.0`，而原始流是 `0.0`。@2fps 就是 2 帧偏移，逐帧恒定。
比对前必须扣掉，否则 20/20 全部"不符"（实测踩过）。

## 用法

```bash
# 起回传服务器（收满指定份数即退）
OUT=/tmp/r.json ROOT=/tmp/cr WANT_REPORTS=1 python3 report.py 8871 &

# 硬解跑 MSE
LIBVA_DRIVER_NAME=msm_drm LIBVA_DRIVERS_PATH=<build目录> \
google-chrome --ozone-platform=wayland \
  --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiIgnoreDriverChecks \
  --autoplay-policy=no-user-gesture-required \
  'http://127.0.0.1:8871/mse_test.html?src=h264_frag.mp4&mime=video/mp4;%20codecs%3D%22avc1.64001f%22&fps=2&n=20&seek=1'
```

参数：`src` 分段流、`mime` MSE codec 串、`fps` 帧率、`n` 采样数、
`chunk` 分块字节、`seek=1` 中途插一次 seek。

## 三个测试页

| 页面 | 覆盖路径 | 关键参数 |
|------|----------|----------|
| `mse_test.html` | MSE `appendBuffer` 分段喂流 | `src` `mime` `fps` `n` `chunk` `seek=1` |
| `net_test.html` | 真实 HTTP 在线源直读 | `url` `fps` `n` `seek=1` |
| `abr_test.html` | ABR 分辨率切换（换 src） | 页内 `srcs` 数组、`PER` 每档帧数 |

### 实测结果（修复版驱动）

真实在线源 `jinjie_265.mp4`（HEVC 1280x720，用户最初报问题的 URL）：
- 直读播放 24 帧：单调递增、零回退，231 次配对
- 加 seek 干扰 24 帧：帧号 0-11 连续，seek 后 86-97 连续，零回退零丢帧

ABR 分辨率切换（640x360 → 1280x720 → 640x360）：
- 三档各采 10 帧，分辨率正确跟随，全部单调、零回退
- 驱动建 3 个会话（`CAPTURE 就绪` 640x368 两次、1280x720 一次），86 次配对

## 编写这类页面的两个坑

**换 src 会取消 pending 的 `requestVideoFrameCallback`。** 紧跟 `v.src=`
立刻重新注册也无效 —— 那时新流还没就绪。必须等 `loadeddata` 事件。
实测症状是整个测试卡住不回传（只建了 1 个会话）。

**MSE 播放推进依赖 `updateend` 驱动 `play()`。** 整段很快 `endOfStream`
之后 `updateend` 不再触发，若那时 `readyState` 还没到 `HAVE_CURRENT_DATA`
就没人调 `play()` 了。实测只采到 1 帧就 timeout。补 `loadeddata`/`canplay`
监听与定时兜底。

## 已知环境限制

- **Chrome MSE 拒收 HEVC**：同一个 `hevc_frag.mp4` 走 `<video src>` 直读
  能正常硬解（实测 131 次配对、帧号单调），走 MSE 报 `sb error` +
  `InvalidStateError`。这是 Chrome MSE 层的策略，与驱动无关。
  HEVC 的浏览器验证请用直读方式。
- **Chrome 软解不支持 HEVC**（专利），所以 HEVC 做不了软/硬解 A-B 对照。
  需要 A-B 对照时用 H.264 素材。
