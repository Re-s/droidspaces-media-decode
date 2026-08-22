# H.264 / HEVC 解码路径：为什么没做，以及接手前必读

VP9 与 VP8 已端到端跑通（硬解与软解逐字节一致）。H.264 / HEVC 的 `vaEndPicture`
目前干净返回 `VA_STATUS_ERROR_UNIMPLEMENTED`，消费者会回退软解，不崩溃。

本文记录停下来的真实原因与已核实的事实，避免接手者重复踩坑或走错方向。

## 1. slice 载荷到底是什么（先纠正一个常见误解）

**VA-API 的 `VASliceDataBufferType` 里是：不含起始码、但含 NAL header 字节的完整 slice NALU，
且保留 emulation prevention 字节。**

证据链（`ffmpeg` 7.1 源码）：

- `libavcodec/h264dec.c:674` —— hwaccel 入口传的是 `nal->raw_data, nal->raw_size`
- `libavcodec/vaapi_h264.c:386-388` —— 这对 buffer/size 原样进 slice data buffer；
  `:349-350` 声明 `slice_data_offset = 0`、`slice_data_flag = VA_SLICE_DATA_FLAG_ALL`
- `libavcodec/h2645_parse.c:92` 与 `:145` —— `nal->raw_data = src`，`src` 指向**起始码之后**
- `libavcodec/h2645_parse.c:448-456` —— `h264_parse_nal_header()` 从 `nal->gb`
  读取 `forbidden_zero_bit` / `ref_idc` / `type`，而 `gb` 初始化自 buffer 起点。
  **NAL header 若不在 buffer 内，这段解析无从进行** —— 这是 header 在内的反证
- `libavcodec/h2645_parse.h:105-110` —— `raw_data` 与 `data` 的区别是前者**保留**
  emulation prevention 字节（未去转义），正是 MediaCodec 需要的原始字节

所以 slice 层要做的只有一件事：**前置 4 字节 `00 00 00 01`**。
不需要重造 NAL header，也不需要重新插入转义字节。

`slice_data_bit_offset`（`vaapi_h264.c:351` = `get_bits_count(&sl->gb)`）是从 buffer
起点算起的 bit 偏移。我们把整段原样转发给 MediaCodec 由它自己解析，**不需要用这个字段**，
不要为补起始码去做偏移换算。

## 2. 真正的阻塞点

不是 slice，是 **参数集（SPS/PPS）**。

daemon 靠起始码定位 NAL 类型来识别参数集（H.264 的 type 7/8、HEVC 的 32/33/34），
把它们累积成 codec-specific data 用 `BUFFER_FLAG_CODEC_CONFIG` 送进 MediaCodec。
而 VA-API **从不传递参数集 NALU** —— 它把 SPS/PPS 解析成
`VAPictureParameterBufferH264` 的字段后就把原始比特流丢掉了。

于是必须反向合成 SPS/PPS 比特流。困难分三层 ——
**其中第一、二层都已被实测排除（第 4 节），真正剩下的只有帧配对（第 5 节第 2 条）
与第三层的 seek/flush**：

**第一层：字段缺失 —— 已有可用实现，见第 4 节。**
`profile_idc`、`level_idc`、VUI（含帧率、色彩空间、`max_dec_frame_buffering`）、
`gaps_in_frame_num_value_allowed_flag` 等在 VA-API 里根本没有对应字段，
只能推断或填默认值。已有一份 515 行的序列化器实测产出被 MediaCodec 接受的
SPS/PPS，可直接复用。

**第二层：slice header 的欠定问题 —— 已实测证明是伪问题，见第 4 节。**
`VAPictureH264` 给的是**最终的参考列表状态**，而不是码流里的
`ref_pic_list_modification` 重排序命令与 `dec_ref_pic_marking` 的 MMCO 命令，
从"最终列表"反推命令序列确实是欠定的。

但这一层**根本不需要解决**：只要把 slice NALU 原样转发（只补起始码），
那些重排序与 MMCO 命令**本来就还在原始字节里** —— VA-API 解析了它们但没有删除。
MediaCodec 自己解析 slice header、自己维护 DPB。
第 4 节的实测（SPS/PPS 合成后 MediaCodec 正常解码出帧）已经确认这一点。
**前提是不要试图重写 slice header。**

**第三层：DPB 状态同步。** MediaCodec 自己维护 DPB，我们无法干预。
客户端 seek/flush 时若不让 MediaCodec 侧同步，两边会失步。daemon 当前**不支持
连接内 reset**（无控制通道），要重来必须重连重新握手。

## 3. 现成参考（三个独立项目收敛到同一解法）

三个项目都是"VA-API 前端 + 吃完整码流的后端"，都反向重建了 SPS/PPS。
这说明重建不是权宜之计，而是这个接口错配下的既定解法。

| 项目 | 后端 | 许可 | 关键位置 |
|------|------|------|---------|
| `Sky1-Linux/libva-v4l2-stateful` | V4L2 **stateful** | **MIT** | `src/h264.c` 的 `h264_detect_profile():81`、`h264_calc_level():107`、`h264_generate_sps():134`、`h264_generate_pps():233`、`h264_handle_slice_data():293`；另有 `src/hevc.c`（807 行，含 VPS/SPS/PPS 重建） |
| `woodyst/rockchip-vaapi` | Rockchip MPP | LGPL-2.1+ | `src/h264.c:44-122` `h264_write_sps()`、`:124-170` `h264_write_pps()`、`:27-42` `emulation_prevent()`、`src/bs.h`（56 行 Exp-Golomb writer） |
| `sfqr0414/rockchip_vaapi_driver` | MPP | 未核实 | `src/util/bitstream.hpp`（完整 Exp-Golomb 写库，实现了 frame_cropping） |

**`libva-v4l2-stateful` 与本项目最同构** —— V4L2 stateful 与 MediaCodec 一样吃完整
Annex B 码流，面对的接口错配完全相同，且它已解决第一层的 profile/level 推断。
许可为 MIT（`meson.build:10` 与 `debian/copyright` 一致），可借鉴。
其 README 标 DEPRECATED，但原因是上游改用 V4L2-M2M 原生路径，不是方案失败。

**不要参考 `nvidia-vaapi-driver`**：它是参数转发（NVDEC 吃参数结构而非码流），
`vabackend.c` 里 grep `sps`/`pps`/`start_code` 零命中，不解决我们的问题。

**MediaCodec 侧无捷径**：输入面只有 csd-0/csd-1（带起始码的 SPS/PPS 二进制）
与 `queueInputBuffer`（access unit 字节流），没有"参数+slice"入口；
Codec2 同为 access-unit 粒度。绕不过参数集重建。

## 4. 已经验证过的部分（重要：不必从零开始）

有一次尝试做到了"SPS/PPS 合成被 MediaCodec 接受"，但因帧配对错位未合入。
结论与产物都留了下来：

**第一层与第二层都已被证明不是障碍。**
一个 515 行的 SPS/PPS 序列化器（Exp-Golomb writer + profile/level 推断 +
RBSP 转义）实测产出 15 字节 SPS 与 9 字节 PPS，**MediaCodec 接受并正常解码**：

```
[dmd-va] H.264: 已送 SPS 15 字节
[dmd-va] H.264: 已送 PPS 9 字节
[dmd-client] 格式块#1: 缓冲 1920x1088 stride=1920 slice=1088
```

这同时说明第 2 节第二层（slice header 重排序/MMCO 反推）**确实是伪问题** ——
只要原样转发 slice 字节、不重写 slice header，那些命令本来就还在码流里，
MediaCodec 自己解析。

产物位置（工作区，未随仓库分发）：
`/home/master/Documents/DSHWK/dmd-vaapi/research/h264-wip/h264_bitstream.c`
公开接口 `dmd_h264_build_sps_nalu()` / `dmd_h264_build_pps_nalu()`。
同目录的 `decode.c.wip` 是当时的接入方式（含已知缺陷，见下）。

**卡住的地方是帧配对，不是码流。** 那次尝试的实测结果：

| | 字节数 | 帧数（÷3110400） |
|---|---|---|
| 硬解 | 457228800 | **147** |
| 软解 | 466560000 | **150** |

两者都是整数帧，所以尺寸正确（1080 而非 1088），是**丢了 3 帧**；
且 `cmp` 显示第 3141541 字节（= 1.01 帧）起就不同 —— **第 2 帧刚开头就错**，
不是尾部丢帧造成的整体偏移。另有 `vaSyncSurface` 返回 38（UNIMPLEMENTED）。

另一次尝试实现完整后帧数达到 148/150，但 **PSNR 仅 11.5dB**（画面错），
同样回退。

### 已被实测排除的四条假设（不要重走）

排除方法统一为：用 `tests/test_dmd_client` 绕过 driver 直连 daemon，隔离变量。

1. **daemon 丢尾部帧 —— 不成立。** 曾怀疑 `decode-daemon.c` 的 output 线程拿到
   EOS 后立刻 `break` 不排空队列。实测 `test1080.h264` 连跑 3 次均为
   `送入单元=161 收到帧=150`，与软解基准 150 帧一致。**daemon 侧无缺陷，
   不要修改 `src/decode-daemon.c`。**
2. **surface 尺寸 1088 —— 不是根因。** H.264 路径 ffmpeg 确实按宏块对齐请求
   1920x1088（VP9 请求精确的 1280x720），日志可证。但 daemon 的 crop 来自
   **码流 SPS** 而非握手参数：握手声明 1080 或 1088，返回几何都是
   `缓冲 1920x1088 / crop=(0,0)-(1919,1079) / 显示 1920x1080`。传 1088 无害。
3. **合成 SPS 缺 VUI —— 不是根因。** 真实 x264 SPS 是 27 字节而合成版仅 11 字节，
   差值几乎全是 VUI。用 `tools/strip_vui.py` 把码流里 5 个 SPS 改写为
   "`vui_present` 置 0、其余比特逐位不变"（27 → **12 字节**）后：软解与原流
   逐字节一致（证明 VUI 不含像素信息），硬解 **150 帧且与软解逐字节一致**。
   **MediaCodec 不需要 SPS 里的 VUI。**
4. **VAImage buffer ID 撞号 —— 不成立。** `image.c:87` 的 `buf_id` 与真 buffer
   共用 `next_buffer_id` 计数器，两者本就不会重号；曾被误读为撞号的
   `image=2/buf=2` 是 image **id** 与 **buf** 分属不同命名空间。

### 比较方法上的陷阱（曾导致误判）

不要直接 `cmp` daemon 的 3133440 字节帧与软解帧：daemon 帧是 **1920x1088 缓冲**，
后 8 行 Y 与对应 UV 区域是**未定义填充**。必须只比较前 1080 行 Y，
且 UV 从偏移 **1920×1088** 起算。用 `tools/cmp_frames.py` 做这个对齐比较。

## 5. 接手建议的顺序

1. **直接复用已有的 `h264_bitstream.c`**（见第 4 节），不要重写序列化器。
2. **最可疑项：PPS 的 `num_ref_idx_l0/l1_default_active_minus1`。**
   这个值只能从 `VASliceParameterBufferH264` 取，但那里的值在
   `override_flag==1` 的 slice 里是**该 slice 自己的**值，不是 PPS 默认值。
   填错会让 `override_flag==0` 的 slice 用错参考帧数量 ——
   症状正是"帧数对、画面糊"，与 11.5dB 吻合。
3. **排查方法：与已知正确答案 diff，不要靠猜。** 把 driver 合成的 SPS/PPS 用
   `DMD_VA_LOG=1` 打成 hex，与真实码流的参数集逐字节比对。
   `test1080.h264` 的真实 PPS 是 `68 eb e3 cb 22`（5 字节），
   真实精简 SPS 由 `tools/strip_vui.py` 生成（12 字节）。
   真实 SPS 逐字段值：`profile_idc=100 level_idc=40 chroma_format_idc=1`
   `log2_max_frame_num_m4=0 poc_type=0 log2_max_poc_lsb_m4=2`
   `max_num_ref_frames=4 w_mbs_m1=119 h_map_m1=67 frame_mbs_only=1`
   `direct8x8=1 frame_cropping=1 crop_L/R/T/B=0/0/0/4`。
4. **多 slice 帧的拼接**：实测 H.264 1080p 每帧约 7 个单元、其中 5-6 个是 VCL。
   这些 slice 必须拼成**一个** access unit 发给 daemon（每个 slice NALU 前各补
   一次起始码），而不是各自单独发送。
5. **先做 H.264 再做 HEVC**。HEVC 多一层 VPS，字段更多，先用 H.264 验证路线可行性。
6. **补 seek/flush**：需要给 daemon 加一类控制消息，或约定客户端 seek 时重建会话。
   VP9/VP8 的全帧独立性让这个缺失在顺序播放下不可见，但 Firefox 拖进度条会错位。

## 6. 已确证可复用的事实

- `vaMapBuffer2` **必须真实现，不能留桩**。libva 只在该槽位为 NULL 时才回落到
  `vaMapBuffer`（`hwcontext_vaapi.c:928` 拿到 UNIMPLEMENTED 会直接失败，
  走不到 `:930` 的兼容分支）。"填满 vtable 以通过 CHECK_VTABLE"与
  "libva 靠 NULL 判断能力"在这一个槽位上真实冲突
- `VAImage.width/height` 报**显示**尺寸，而 `offsets[1]` 必须用
  `stride × slice_height`（**缓冲**高）。1080p 实测 `offsets=[0, 2088960]` = `1920×1088`。
  用显示高 1080 算会让色度平面偏移 `stride×8` 字节，症状是绿边与色度错位
- 验收命令必须带 `-hwaccel_output_format vaapi`。只写 `-hwaccel vaapi` 时
  ffmpeg 会自动把帧下载成软件格式，滤镜链报
  "Impossible to convert between the formats"，看起来像 driver 的错
- `dmd_client` 的 `send_unit` 不内部排空：批量提交多帧前必须先 `next_frame`，
  否则 daemon 会静默丢单元

## 第二轮实测修正（IP 切换后重跑）

上一轮的两条结论是错的，已在 `research/I-2b-decode-path.md` §5.2 详细更正：

1. **PSNR 11.5dB 是测量错误。** 我按 1088 帧长读输出，但 `hwdownload` 出来的是
   1920x1080（`size % 3110400 == 0` 可自检）。修正几何后真实值 **22.87dB**。
   连带推翻"surface 取了 1088"这条曾被标为最可疑的线索 —— 几何本来就对。
2. **帧数是 148 帧整**，与 daemon 回收数一致，driver→ffmpeg 一帧没丢。

**真根因已确证**：PPS 的 `num_ref_idx_l0/l1_default_active_minus1` 取自
`VASliceParameterBufferH264`，但参数集必须在首个 VCL 之前送，那时只有 IDR 的
slice param —— I slice 的 `num_ref_idx` 恒为 0。运行时实测：

```
合成 PPS: [00 00 00 01 68 ef 8f 2c 8b]  l0=0 l1=0
真实 PPS: [00 00 00 01 68 eb e3 cb 22 c0]  l0=2 l1=0
```

后果：未 override 的 P/B slice 只用 1 个参考帧 → 前 2 帧正确、第 3 帧起持续
偏差、每个 IDR 处短暂恢复。逐帧 PSNR（148 帧里 5 帧 inf，恰为 5 个 IDR）
与 md5 逐帧指纹（错误内容在软解输出里不存在，故非帧序问题）双重印证。

第一轮离线自测显示"PPS 逐字节一致"是**假阳性**：测试用例手填了 l0=2。
教训：自测输入必须取自真实运行时快照。

**推荐修法（比我试的暂存方案简单）**：PPS 默认值改用 SPS 的
`num_ref_frames`。任何合法 slice 的 `num_ref_idx_active` 都 ≤ 它，不会造成
参考帧不足（不足才解错）。逐字节一致不是目标，解码正确才是。
