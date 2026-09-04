# 版本陷阱与升级理由

这里收纳各版本的已知缺陷、症状与根因。从 README 移出的原因是：这些内容对
**正在用旧版**的人很关键，对新用户则是噪音 —— 装最新版就都不会遇到。

按"你在用哪一版"查表。完整变更记录见 [`../CHANGELOG.md`](../CHANGELOG.md)。

## 速查

| 你在用 | 症状 | 修复版本 |
|---|---|---|
| 0.4.1 及更早 | `-f null -` 自检挂死，26 帧后 `internal decoding error` | 0.4.2 |
| 0.4.2 及更早 | 播非 1080p 绿屏（1080p 正常） | 0.4.3 |
| 0.4.3 及更早 | 浏览器拖进度条 / 切清晰度卡数秒 | 0.4.4 |
| 0.4.5 及更早 | Chrome 硬解开头几帧顺序错乱 | 0.4.6 |

## 0.4.1 及更早：自检命令挂死

```sh
# 这条命令在 0.4.1 上会挂死（约 40s 超时后报错）
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
  -hwaccel_output_format vaapi -i in.mp4 -f null -
```

26 帧后 `internal decoding error`，根因是 CAPTURE 背压死锁，0.4.2 修复。

在旧版上这个现象容易被误判成"驱动不可用"。旧版请改用落盘形式自检：

```sh
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
  -i in.mp4 -pix_fmt yuv420p -f rawvideo out.yuv -y
```

## 0.4.2 及更早：非 1080p 绿屏

**1080p 自检通过不代表非 1080p 没问题** —— 旧版在 1080p 上完全正常，
这也是该缺陷长期未被发现的原因（原有四条回归流全是 1920x1080，
恰好等于 msm_vidc 打开设备时的默认 CAPTURE 几何，永不触发出错分支）。

根因是 CAPTURE 残留几何。`G_FMT(CAPTURE)` 总返回 msm_vidc 的默认
`1920x1088`，不随 `S_FMT(OUTPUT)` 联动。驱动虽然用 OUTPUT 协商值覆盖了宽高，
但 `bytesperline` 与 `sizeimage` 驱动不回填，仍是 1080p 的值；而原有保护
只检查下限（`if (d->stride < d->w * bpp2)`，播 1280x720 时 `1920 < 1280`
不成立），于是错误 stride 被保留，造成双重损坏：

- 按 `stride=1920` 取 1280 宽的帧，**每行错位 640 字节**
- 用它反推 slice_height 得 `cap_size*2/(1920*3) = 492`，把真实的 736 压成 492，
  于是每帧截断（`帧需 1416960 字节 > dumb buffer 1413120 字节`）

Firefox 实测 356 次导出里 351 次带着坏几何。

0.4.3 给 stride 加了上限校验（Venus 的 CAPTURE stride 按 128 对齐，合法值必落在
`[align(w,128), align(w,128)+128)`），`cap_size` 按纠正后几何重算。

同版还修了起播首帧纯绿：NV12 里 `UV=0` 不是无色而是最大色偏，经限制范围
BT.601 转 RGB 得 `R≈0, G≈135, B≈0`（中性值是 128）。Firefox 会在解码之前就
`vaExportSurfaceHandle` 拿 fd 建纹理，于是看到 surface 分配时 memset 出来的
那一整块零。改为 Y 填 0、UV 填 `0x80` 后，230 次导出 3 次纯绿变成 247 次导出
0 次。

## 0.4.3 及更早：seek 与切清晰度卡数秒

浏览器里拖进度条或切清晰度会卡住数秒，且**每次都卡**。

根因是 `PORT_SETTINGS_CHANGED_INSUFFICIENT` 从未被真正处理：每次触发都要
白等 2s flush + 5s SyncSurface 超时，然后重建会话，循环往复。

0.4.4 按厂商 OMX 的正规序列重配：先 `FLUSH_CAPTURE` 并等 `FLUSH_DONE`，
再重配 CAPTURE。详见 [`../CHANGELOG.md`](../CHANGELOG.md) 的 v0.4.4。

## 0.4.5 及更早：Chrome 开头几帧顺序错乱

Chrome 硬解时画面前后帧跳跃，开头几帧顺序错乱、之后恢复正常流畅；
同一文件软解正常，网络流与本地文件表现一致。

两处独立根因：Chrome 采到未写入的 surface（影响所有编码），以及 H.264 会话
首份 PPS 误用了 I slice 的 `num_ref_idx=0`（只影响 H.264 的前两帧）。

0.4.6 修复。完整的 A-B 验证数据、被排除的误判原因、以及三条验证方法上的
教训见 [`../CHANGELOG.md`](../CHANGELOG.md) 的 v0.4.6。

## 编解码器层面的固有限制

不随版本变化，属能力边界而非缺陷。

**HEVC 带 `st_ref_pic_set` 的码流无法支持**（SPS 里
`num_short_term_ref_pic_sets > 0`）。VA-API 只给个数不给内容，驱动无法复现，
此时 `vaEndPicture` 返回 `UNIMPLEMENTED` 让上层回落软解。
实测 x265 默认输出 0，常见码流不受影响。

**HEVC Main10 / VP9 Profile2 不出帧。** 固件能识别 10bit 但持续报
`INSUFFICIENT`，属固件限制。

**AV1 像素未通过。** 帧数与 dav1d 一致但像素不对，默认不声明，
需 `-DDMD_ENABLE_AV1` 才编入。完整测绘见
[`av1-v4l2-status.md`](av1-v4l2-status.md)。

**MPEG-2 固件 `SYS_ERROR`。** 合成的比特流与原始流逐字节一致，但固件拒绝，
默认不声明，需 `-DDMD_ENABLE_MPEG2`。

## 4K 不要开硬解

见 [`performance-and-roadmap.md`](performance-and-roadmap.md)。
简言之：4K 的收益被 CPU 侧的拷贝与转色吃掉，实测不如软解。
