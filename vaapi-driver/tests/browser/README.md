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

## 已知环境限制

- **Chrome MSE 拒收 HEVC**：同一个 `hevc_frag.mp4` 走 `<video src>` 直读
  能正常硬解（实测 131 次配对、帧号单调），走 MSE 报 `sb error` +
  `InvalidStateError`。这是 Chrome MSE 层的策略，与驱动无关。
  HEVC 的浏览器验证请用直读方式。
- **Chrome 软解不支持 HEVC**（专利），所以 HEVC 做不了软/硬解 A-B 对照。
  需要 A-B 对照时用 H.264 素材。
