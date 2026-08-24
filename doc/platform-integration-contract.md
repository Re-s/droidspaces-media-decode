# 平台接入契约（Platform Integration Contract）

面向 DroidSpaces 平台工程师。本文只描述**平台侧需要提供什么**，以及每项要求的实测依据与验证方法。

本文所有"实测"结论的取证设备与环境见 `doc/verified-platform-facts.md`。
未经实测的内容一律标注为"待平台确认"或"未验证"，不做推断填充。

---

## 1. 一句话概述

> 🌐 **English version: [platform-integration-contract.en.md](platform-integration-contract.en.md)**

本组件让 DroidSpaces 容器内的标准 Linux 应用（ffmpeg / Firefox / Chrome）通过 **VA-API** 使用 Android 宿主的 **MediaCodec 硬件解码**，应用无需改动；它由两部分组成 —— 宿主侧的 `decode-daemon` 与容器侧被 libva `dlopen` 的驱动 `msm_drm_drv_video.so`。

**平台需要做四件事**：

1. 把一个宿主目录 bind mount 进容器（§2.1）
2. 以正确的 SELinux domain 启动 daemon（§2.2）
3. 把 `/dev/dri/renderD128` 透传进容器（§2.3）
4. **补 SELinux allow 规则**：让媒体**服务端域**能把 binder 句柄
   transfer 给 daemon 所在的 domain（§2.2）

不需要任何网络或端口配置 —— 通道是 Unix socket，不上网络栈。

> ⚠️ **第 4 项既容易漏排，也容易搞反方向。** 前三项做完后，
> §6 验证清单的第 1–7 步会**全部通过**，只有第 8 步（真实解码）失败 ——
> 因为 daemon 要到创建 codec 时才去取解码器，握手阶段不碰。
> 也就是说"看起来都装好了"并不代表能解码。
>
> 关于搞反：规则的 subject 是**服务端域**
> （`allow mediacodec droidspacesd binder { call transfer }`），
> 不是 daemon 自己的 domain。`binder { transfer }` 按 sender 判定，
> receiver 是 permissive 也盖不住。完整推理与已验证的规则集见 §2.2。

---

## 2. 平台必须提供的三件事

### 2.1 共享目录的 bind mount

**要求**

| 项 | 值 |
|---|---|
| 宿主目录 | `/data/local/tmp/dmd/`（示例路径，实际位置由平台决定） |
| 容器挂载点 | `/run/dmd/` |
| 挂载对象 | **目录**，不是单个 socket 文件 |
| 容器内实际端点 | `/run/dmd/decode.sock` |
| 读写 | 需可读写（客户端要 `connect`） |

容器侧路径是驱动的编译期默认值 `DMD_DEFAULT_SOCK`（`vaapi-driver/src/dmd_client.h` 的 `DMD_DEFAULT_SOCK`），文件名 `decode.sock` 是 daemon 目录模式下的固定名 `DAEMON_SOCK_NAME`（`src/decode-daemon.c` 的 `DAEMON_SOCK_NAME`）。两侧必须一致，改一边就连不上。

**为什么必需**

容器与宿主的 namespace 关系决定了只有这一条传输通道对两类容器都成立：

- 路径式 Unix socket **不属于 net namespace**，只受 mount namespace 约束，因此 bind mount 就能跨界。
- TCP loopback 与 abstract socket 都属于 net namespace，NAT 型容器 netns 独立（实测 `4026535650` vs 宿主 `4026531937`），`127.0.0.1:20003` **不可达**、容器内可见的 abstract socket 数为 **0**（宿主与 host 型容器均为 31）。
- 实测已跑通完整链路：宿主建 Unix socket → bind mount 进容器 → 容器 `connect` → 通过 `SCM_RIGHTS` 收 memfd → `mmap` 读到宿主写入的内容。
- DroidSpaces 平台自身就在用同一模式：宿主 `/data/local/tmp/anland-<hash>.sock` bind mount 到容器 `/run/display.sock`，实测两侧 inode 相同，确认是同一文件。

**为什么必须挂目录而不是单个 socket 文件**

这是 `bind()` 与 bind mount 的 inode 语义冲突，不是偏好问题：

- `bind()` 只能**创建新 inode**，无法绑到已存在的文件。所以 daemon 每次重启，socket 文件都是**新 inode**。
- bind mount 绑定的是 **inode 而非路径**。
- 结果：若平台挂载的是单个 socket 文件，daemon 一重启，容器侧挂载点仍指向那个已经孤立的旧 inode，`connect` 立刻变成 `ECONNREFUSED`，**必须重新挂载**。更麻烦的是此时宿主两侧 `stat` 看到的 inode 会一致（都是孤立 inode），很容易误判为"挂载还好着"。

挂目录没有这个问题：目录 inode 稳定，里面的 socket 换 inode 不影响挂载。平台只挂一次，daemon 随便重启，容器一直连 `/run/dmd/decode.sock`。这一约束与对策在 `src/decode-daemon.c` 的 `--sock` 目录模式分支（搜 `S_ISDIR`）与 flock 判活段（搜 `LOCK_EX`）的注释里有对应说明。

**验证方法**

比较两侧 inode，相同即确认是同一文件：

```bash
# 宿主（Android 侧）
stat -c '%i %F %n' /data/local/tmp/dmd/decode.sock
# 容器内
stat -c '%i %F %n' /run/dmd/decode.sock
```

期望：两条输出的 inode 号**完全相同**，类型均为 `socket`，例如

```
1234567 socket /data/local/tmp/dmd/decode.sock
1234567 socket /run/dmd/decode.sock
```

再验证真能连上（容器内）：

```bash
python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('/run/dmd/decode.sock');print('connect ok')"
```

期望输出 `connect ok`。若得到 `ConnectionRefusedError`，最可能是 daemon 重启后挂载点指向了孤立 inode（见上文），或 daemon 未在运行。

### 2.2 daemon 以正确的 SELinux domain 启动

**要求**

```bash
mkdir -p /data/local/tmp/dmd
runcon u:r:droidspacesd:s0 /path/to/decode-daemon --sock /data/local/tmp/dmd
```

`--sock` 传**目录**：daemon `stat` 到它是目录时，会在其中建固定名
`decode.sock`。目录不存在时 daemon 会**自动创建**（0755），
并在日志里写明 `--sock 目录不存在，已创建`。

上面仍显式写了 `mkdir -p`，因为平台通常要先建好目录才能配置 bind mount ——
挂载点必须在容器启动时就存在。

> **两个已修的坑，供参考**（v0.3.0 发布前修掉）：
> - **尾斜杠**：早前传 `/data/local/tmp/dmd/` 会拼出
>   `/data/local/tmp/dmd//decode.sock`。功能正常，但日志里的双斜杠与本文
>   §6 第 2 步要匹配的字符串对不上，容易误判成没生效。现已剥除尾斜杠。
> - **目录不存在时静默降级**：早前会把该路径直接当成 socket **文件名** bind，
>   静默造出一个单文件 socket。单文件挂载在 daemon 重启后必然失效
>   （inode 变了），而平台当时拿不到任何警告。现在会自动建目录；
>   若路径以 `.sock` 结尾则视为确实想要文件模式，此时会打印告警说明
>   重启后挂载会断。

daemon 的进程形态（`README.md`"daemon 的定位"一节）：前台运行、不自我 daemonize、日志走 stderr、`SIGTERM` 优雅退出。**判活请用监听端点而不是 PID** —— PID 会反复变化。

**为什么必需**

在 Android 侧创建 socket 文件受 SELinux 约束，实测：

- 用 `su` 直接启动会跑在 `u:r:ksu:s0`，`bind()` 得到 **`EACCES`**。
- 以 `runcon u:r:droidspacesd:s0` 启动则 `bind()` 成功。
- 容器进程自身的上下文也是 `u:r:droidspacesd:s0`。

建 socket 这一步**不需要新增 SELinux 规则** —— 策略已允许，只是要用 DroidSpaces 自己的 domain。daemon 在 `bind()` 拿到 `EACCES` 时会直接打印这条提示（`src/decode-daemon.c` 的 `bind()` 失败提示（搜 `runcon u:r:droidspacesd`））。

### ⚠️ 但仅切 domain 还不够：需要平台补一条 allow 规则

这是当前**唯一阻塞 Unix socket 通道交付**的问题。两个可用身份各只有一半权限：

| 启动身份 | 能 `bind()` socket | 能用 MediaCodec |
|---|---|---|
| `su`（`u:r:ksu:s0`） | ✗ 各处 `EACCES` | ✓ |
| `runcon u:r:droidspacesd:s0` | ✓ | ✗ SIGABRT |

`droidspacesd` 下崩溃的现场：tombstone 栈顶 `Codec2Client::GetServiceNames` → `CCodec::allocate`，报 `Check failed: serviceManager Hardware service manager is not running.`

三组对照实验确认这与传输方式**无关**：

| 身份 + 传输 | 结果 |
|---|---|
| `ksu` + TCP | 正常解码（但该 domain 无权 bind socket） |
| `droidspacesd` + TCP | **同样 SIGABRT** ← 证明与 Unix socket 改动无关 |
| `droidspacesd` + Unix socket + **SELinux permissive** | **正常解码 9 帧** |

第三行是关键：把 SELinux 切成 permissive、其他条件完全不变就能正常工作，说明 **Unix socket 通道本身是正确的**，缺的只是一条 allow 规则。

> ⚠️ **v0.3.1 修正：规则的方向此前搞错了。** 下文一度把所需规则写成"允许
> `droidspacesd` 访问 hwservicemanager / Codec2"，即以 `droidspacesd` 为
> subject。**那样加不会有任何效果** —— `droidspacesd` 是永久 permissive 域，
> 以它为 subject 的检查本来就不阻断。
>
> 真正缺的是反方向：服务端域要把 binder 句柄 **transfer 给** `droidspacesd`，
> 而 `binder { transfer }` 按 **sender**（服务端域，enforcing）判定。denial
> 被 `dontaudit` 静默，所以全程看不到 avc 行，这正是误判的来源。
>
> 一锤定音的对照：`dumpsys -l`（纯枚举）两域都成功，`dumpsys media.player`
> （需回传句柄）在 `droidspacesd` 下 `FAILED_TRANSACTION`、`ksu` 下正常，
> 同一模式在 system binder 与 hwbinder 两条总线一致复现。失败链的第一因是
> `getService ... EX_TRANSACTION_FAILED for android.hardware.media.omx@1.0::IOmx`
> → `Cannot obtain IOmx service` → ACodec `-19`，SIGABRT 是 Codec2 回退时的
> 二次现象，不是根因。

**已排除的替代方案**（都实测过）：

| 方案 | 结论 |
|---|---|
| 用 root / `su` | 无效 —— 两个 domain 本来都是 uid 0，SELinux 是 MAC，不看 uid |
| DroidSpaces `enable_hw_access=1` | 无效 —— 只透传 `/dev` 节点、不改 domain，且作用于容器而非宿主侧的 daemon |
| `untrusted_app` domain | 走不通 —— 无法执行 `shell_data_file` 标签的二进制，`chcon` 改标签被拒 |
| 其余 domain（`magisk`/`init`/`shell`/`system_server`/`mediaserver`/`media_codec`/`hal_codec2_default` 等 11 个） | 均无法同时满足"可切入"与"可 bind" |
| DroidSpaces `selinux_permissive=1` | 有效，但那是**把宿主 SELinux 整体切成 permissive**（帮助原文：`Set host SELinux to permissive mode`），全系统关防护，不可作为交付形态 |

**所需规则**（subject 是服务端域，不是 `droidspacesd`）：

```
allow mediacodec   droidspacesd binder { call transfer }
allow mediacodec   droidspacesd fd use
allow mediametrics droidspacesd binder { call transfer }
allow mediametrics droidspacesd fd use
```

`mediacodec` 域即 `media.hwcodec`，IOmx 的提供者。注意 `hal_codec2_default`
类型在测试设备的策略里**不存在**（Codec2 由 `mediacodec` 域提供），CIL 里
引用不存在的类型会导致整个策略编译失败，不要照抄进去。

这类配置平台已经做过，而且是**完全同一形状**的规则 —— PulseAudio 能出声就
靠 `sepolicy.rule` 里这两条：

```
allow audioserver droidspacesd binder { call transfer }
allow mediaserver droidspacesd binder { call transfer }
```

所以硬解要的只是补上 OMX 对应的服务端域，属于既有惯例的自然延伸，不是新机制。

> **状态（v0.3.1）**：平台侧已实现并验证通过。规则加在
> `Android/app/src/main/assets/boot-module/sepolicy.rule` 与
> `init/android-service/binary-configuration/droidspaces_binary.cil` 两个载体
> （平台双轨惯例）。补规则后端到端逐字节验证：150 帧 1080p，`vainfo` 报出驱动
> 版本与 6 个 VLD profile，输出与软解 `md5` 相同。
>
> 补规则后 `hwservice_manager { find }` 的 denial **依然存在**且标
> `permissive=1`（IOmx / IAllocator / IMapper 各一条），但不再阻断 —— 因为
> `find` 的 subject 是 `droidspacesd` 自己，permissive 能放行。这条现象容易
> 让人以为规则没生效，实际是正常的。

**验证方法**

```bash
ps -AZ | grep decode-daemon
```

期望进程标签为 `u:r:droidspacesd:s0`。

同时检查 daemon 的启动日志（stderr），期望连续两行：

```
--sock 是目录，实际监听 /data/local/tmp/dmd/decode.sock
listening on /data/local/tmp/dmd/decode.sock
```

`listening on ...` 是启动成功的标志行，文本稳定，可被托管脚本直接匹配（TCP 分支为 `listening on <端口>`，格式一致）。**只有这行出现才算启动成功**：daemon 在 `bind`/`listen` 失败时不会打印它。

**部署顺序约束**

| 步骤 | 动作 |
|---|---|
| 1 | daemon 先启动，建好 socket 文件 |
| 2 | 平台**再** bind mount 目录进容器 |
| 3 | 挂目录时，daemon 后续重启无需重挂 |

若平台出于某种原因只能挂单文件，则第 3 步变为"daemon 每次重启后必须重新挂载"。

> **待平台确认**：daemon 二进制的放置位置、托管配置的声明形式（启动命令 / bind mount 项如何写）、启动时机（daemon 依赖 Android media 服务就绪）、崩溃重启与日志收集策略。这些属于平台内部实现，本项目不做假设。

### 2.3 `/dev/dri/renderD128` 设备透传

**要求**

| 项 | 值 |
|---|---|
| 设备节点 | `/dev/dri/renderD128`（DRM 渲染节点） |
| 属主 / 属组 | `root:droidspaces-gpu` |
| 权限 | `crw-rw----` |
| 容器内用户 | 需在 `droidspaces-gpu` 组内 |

host 型容器已有该节点；NAT 型容器最初没有，**平台配置后才有**。

**为什么必需**

两条都是硬依赖：

1. **libva 靠它发现驱动**：libva 用 `DRM_IOCTL_VERSION` 从 `/dev/dri/renderD128` 取内核驱动名，实测为 `msm_drm`；映射表里没有 msm 条目，于是走 fallback 原样使用该名字，只尝试唯一一个文件名 `/usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so`。没有这个节点，驱动根本不会被加载（`vaapi-driver/README.md`"无感发现机制"）。
2. **可导出 surface 依赖它**：驱动把 surface 分配在 msm_drm 的 dumb buffer 里（`DRM_IOCTL_MODE_CREATE_DUMB` + `MAP_DUMB`，`vaapi-driver/src/decode.c` 的 dumb buffer 分配），`vaExportSurfaceHandle` 由此导出 dmabuf fd（`vaapi-driver/src/export.c`）。**Firefox 与 Chrome 硬解只走这个入口**，拿不到 fd 就静默回落软解。fd 不可用时驱动会退回普通 heap（同文件的导出路径），ffmpeg 的 `vaDeriveImage` 路径仍能工作，但浏览器路径会失效。

**验证方法**

容器内：

```bash
ls -l /dev/dri/renderD128 && id
```

期望 `crw-rw---- 1 root droidspaces-gpu ...`，且 `id` 的输出里包含 `droidspaces-gpu` 组。

---

## 3. 两类容器的差异与支持矩阵

实测的 namespace 归属差异：

| | host 型 | NAT 型 |
|---|---|---|
| net namespace | 与宿主共享（`4026531937`） | 独立（如 `4026535650`） |
| IP | 直接持有 `wlan0` 真实 IP | `eth0` 172.28.x.x/16，网关 172.28.0.1 |
| `127.0.0.1:20003` 连宿主 | 可达 | **不可达** |
| 可见 abstract socket 数 | 31（与宿主一致） | **0** |
| mnt / pid / uts / ipc / cgroup ns | 均隔离 | 均隔离 |

由此得到传输通道的支持矩阵：

| 传输 | 依赖的 namespace | host 型 | NAT 型 | 说明 |
|---|---|---|---|---|
| **路径式 Unix socket**（`--sock`） | 仅 mount ns（靠 bind mount 跨界） | ⚠️ 见下 | ⚠️ 见下 | **唯一对两类容器都成立的通道，推荐** |
| TCP `127.0.0.1:20003` | 共享 net ns | ✅ | ❌ | NAT 型 netns 独立，loopback 不通 |
| abstract socket（`@` 前缀） | 共享 net ns | ✅ | ❌ | NAT 型可见数为 0 |

> ⚠️ **Unix socket 那行的两个 ⚠️ 是指：通道机制本身已验证可用，但当前
> 在 SELinux Enforcing 下起不了作用** —— 缺 §2.2 第 4 项那条 allow 规则。
>
> 实测状态：
> - **Permissive 下**：两类容器都通，十二条流逐字节比对 12/12 一致
> - **Enforcing 下**：daemon 能建 socket、能握手，但创建 codec 时 SIGABRT，
>   解码返回 0 帧（`internal decoding error`）
>
> 规则补上后这两格才是真正的 ✅。在那之前，实际可用的通道是 TCP
> （仅 host 型容器），驱动会自动退回。

驱动侧的端点选择逻辑（`vaapi-driver/src/decode.c` 的 `session_open`）：

- `DMD_ENDPOINT=unix:<路径>` 或 `DMD_ENDPOINT=tcp:<端口>` 可显式指定；
- 不设时自动探测：`/run/dmd/decode.sock` 存在且是 socket 就用它，否则走 TCP；
- Unix socket 连接失败时**自动退回 TCP** —— 平台还没挂载时，host 型容器仍能工作。

**平台结论**：只提供 Unix socket 通道即可覆盖两类容器。不要为 NAT 型容器去做端口转发（见第 5 节）。

---

## 4. 可选优化：memfd 零拷贝（SHM 传输）

TCP 模式每帧经过两次内核拷贝；SHM 模式把帧数据放进 `memfd`，socket 只传 20 字节控制消息。

**取证环境为 `tests/test_dmd_client.c`（独立测试程序）**，实测 1080p 收益：稳态窗口吞吐 +12.9%、daemon CPU **−28.6%**，两种模式解出的帧逐字节完全一致。

> ⚠️ **不要把这组数字当作驱动侧的预期收益。** 它出自独立测试程序，
> 而驱动被 `dlopen` 进真实消费者（ffmpeg / 浏览器）后的 SHM 路径
> **至今没有端到端跑通**（见下方说明）。真实收益未知，可能更低。

⚠️ **它只在 host 型容器可能可用，NAT 型容器不可用。**

> **勘误**：本文档早前版本称"它在两类容器都可用，因为不依赖 net namespace"。**那个结论是错的**。SHM 的 memfd 交接并不走 §2.1 那条路径式 Unix socket，而是 daemon 另开一个 **abstract** socket（`src/decode-daemon.c`，搜 `sun_path[0] = 0`），驱动再去连它。abstract socket **属于 net namespace** —— NAT 型容器 netns 独立，连不上那个交接通道，`DMD_WANT_SHM=1` 在那里必然交接失败再降级，每建一次会话白等一次超时。
>
> 教训：**控制通道能跨 netns，不等于 SHM 交接通道也跨得过去。** 要让零拷贝在 NAT 容器可用，需把 memfd 交接改走同一条路径式 Unix socket（在已有连接上传 `SCM_RIGHTS` 即可，不必另开 socket）。那是一项独立改动，尚未实施。

已实测确认的部分：路径式 Unix socket 跨 mount namespace 可用、`SCM_RIGHTS` 能跨界传 fd、memfd 跨 namespace 可映射（宿主写入的内容容器侧 `mmap` 后能读到）。这些是零拷贝的必要条件，但当前实现没有用上那条通道。

**平台侧无需为此做任何配置** —— 这一项是后续优化，不是接入要求。

⚠️ **当前默认关闭。** 驱动侧需 `DMD_WANT_SHM=1` 且已走 Unix socket 才请求 SHM（`vaapi-driver/src/decode.c` 的 `want_shm` 判定（搜 `DMD_WANT_SHM`））。

这条路**驱动侧从未真正启用过**（`want_shm` 长期硬编码为 0），只有 `tests/test_dmd_client.c` 走过：实测 150 帧、与 TCP 前 20 帧逐字节一致、无 fd 泄漏。但那是独立测试程序，不是驱动被 `dlopen` 进消费者进程的真实环境。

尚未验证的风险是浏览器沙箱（Firefox RDD / Chrome GPU 进程的 seccomp 过滤）能否接收 `SCM_RIGHTS`。本项目有先例：一份源码级结论称 RDD 沙箱对 `SYS_SOCKET` 一律返回 `EACCES`，实测却跑通了 713 帧 —— 这类判断只能靠实测。

> **勘误**：本文档早前版本称"走 Unix socket + SHM 时 daemon 在 memfd 交接后不再服务、根因未定位"。**那个归因是错的**。真实原因是 §2.2 的 SELinux domain 权限问题（daemon 在 `droidspacesd` 下无权访问 Codec2，在 `CCodec::allocate` 处 SIGABRT），与 SHM 无关 —— 当时日志里的传输模式其实是 TCP。

所以这一项列为"平台不必操心的后续优化"，不是接入要求。

---

## 5. 不需要平台做的事

明确列出以避免过度配置：

| 项 | 结论 | 依据 |
|---|---|---|
| ~~新增 SELinux 策略~~ | **⚠️ 需要，见 §2.2**（v0.3.1 已由平台实现） | 建 socket 确实不需要新规则。需要的是让**服务端域**能把 binder 句柄 transfer 给 `droidspacesd`（`allow mediacodec droidspacesd binder { call transfer }` 等）。早前两版都写错过：先误列为"不需要"，后又把 subject 写成 `droidspacesd` 自己 |
| 端口转发 / iptables / NAT 规则 | **不需要** | Unix socket 通道不经过网络；NAT 型容器不再需要 TCP 可达性 |
| 任何网络配置（DNS、路由、防火墙放行） | **不需要** | 传输不走网络栈 |
| 透传 `/dev/ashmem` | **不需要** | 该节点已透传（`root:droidspaces-gpu`），但本项目**不使用**它 —— Android 11+ 已被 memfd 取代，且 memfd 方案更优 |
| 提供 tmpfs 共享点（`/dev/shm`、`/run`） | **不需要** | 它们是容器 mount namespace 内部的挂载点，Android 侧挂载表里不存在 |
| 用共享目录传帧数据 | **不需要，且不应做** | `/data/local/tmp` ↔ 容器 `/tmp` 是 f2fs 磁盘背书，实测单帧 0.374 ms 比 memfd 的 0.227 ms 慢 65%；控制面（日志、码流文件）可用 |
| 为容器安装 VA-API 驱动到特殊路径 | **不需要** | 驱动装在标准 `dri` 目录，libva 自动发现，零环境变量 |
| 进程管理模块（KSU / Magisk） | **不需要** | 该方案已放弃；daemon 设计为被平台托管的前台进程 |
| 抬高 daemon 权限或让它常驻 root shell | **不需要** | 只需正确的 domain，见 2.2 |

> **待平台确认**：socket 文件权限的收紧目标。daemon 目前 `chmod 0666`（`src/decode-daemon.c` 的 `listen()` 与权限设置（搜 `chmod`）），注释里已标注这是"先跑通"的放宽值，真实部署应收紧到特定 gid（参考 `/dev/dri` 用的 `droidspaces-gpu`）。**平台希望用哪个 gid、由谁设置属组**需要确认后再改代码。
>
> **待平台确认**：容器内 `/run/dmd/` 挂载点目录由谁创建（`/run` 在容器内是 tmpfs），以及挂载在容器启动流程的哪个阶段发生。

---

## 6. 验证清单

平台配置完成后按顺序执行。任一步失败就停在那一步，不要往后跳。

| # | 位置 | 命令 | 期望输出 |
|---|---|---|---|
| 1 | 宿主 | `ps -AZ \| grep decode-daemon` | 进程存在，标签 `u:r:droidspacesd:s0` |
| 2 | 宿主 | 查 daemon stderr 日志 | 含 `listening on /data/local/tmp/dmd/decode.sock` |
| 3 | 宿主 | `stat -c '%i %F' /data/local/tmp/dmd/decode.sock` | `<inode> socket`（⚠️ 经 `adb` 传递时见下方注意事项） |
| 4 | 容器 | `stat -c '%i %F' /run/dmd/decode.sock` | inode 与第 3 步**完全相同**，类型 `socket` |
| 5 | 容器 | `python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('/run/dmd/decode.sock');print('connect ok')"` | `connect ok` |
| 6 | 容器 | `ls -l /dev/dri/renderD128 && id` | `crw-rw---- 1 root droidspaces-gpu`，`id` 含 `droidspaces-gpu` |
| 7 | 容器 | `vainfo` | `Driver version: DroidSpaces MediaCodec VA-API driver ...` 与 6 个 `VAEntrypointVLD` profile，exit code 0 |
| 8 | 容器 | 见下方 ffmpeg 端到端命令 | 正常写出 `out.yuv`，日志含 `Initialised VAAPI connection: version 1.22` 与本驱动 vendor 串 |

第 8 步的完整命令（`-hwaccel_output_format vaapi` 必须显式给，否则 ffmpeg 会自动下载成软件格式并报 "Impossible to convert between the formats"）：

```bash
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
       -hwaccel_output_format vaapi \
       -i in.h264 -vf hwdownload,format=nv12 -f rawvideo -y out.yuv
```

排查时的两个辅助开关：

```bash
DMD_VA_LOG=1 <消费者>        # 打开驱动侧日志（默认静默）
DMD_ENDPOINT=unix:/run/dmd/decode.sock <消费者>   # 显式指定端点，绕过自动探测
```

> 在无显示的 SSH 会话里裸跑 `vainfo` 会先打印 Wayland/X11 连接失败，随后自动落到 drm 路径成功。那两行与本驱动无关。
>
> ⚠️ 第 7、8 步通过**不代表浏览器也能硬解** —— ffmpeg 走 `vaDeriveImage`，浏览器走 `vaExportSurfaceHandle`，是两条不同的出口。浏览器验证还需要容器内的桌面会话与额外环境（见 `vaapi-driver/README.md`"浏览器（Firefox）"一节），那部分属于容器镜像与桌面环境配置，不在本契约范围内。

### 在 Android 侧执行这些命令时的注意事项

上表的命令按"在目标侧的 shell 里直接执行"书写。经 `adb` 从外部传入时有两个坑，
都实测踩过：

**一、内层引号会被拆开。** 第 3 步经 `adb shell su -c` 传递时：

```bash
# ✗ 坏：外层双引号先被本机 shell 处理，'%i %F' 的空格把参数拆成两个
adb shell su -c "stat -c '%i %F' /data/local/tmp/dmd/decode.sock"
#   → stat: '%F': No such file or directory
#     1270438

# ✓ 好：转义内层引号
adb shell "su -c \"stat -c '%i %F' /data/local/tmp/dmd/decode.sock\""
#   → 1270438 socket

# ✓ 也好：避开空格，分两次取
adb shell su -c "stat -c %i /data/local/tmp/dmd/decode.sock"
```

Android 的 `stat` **支持** `%i` 与 `%F`，报错纯粹是引号层级问题，
不要误判成"Android stat 不支持该格式符"。
判类型也可以直接用 `ls -l`（socket 显示为 `s` 开头，如 `srw-rw-rw-`）。

**二、Android 自带 shell 不支持部分 POSIX 语法。** `for x in a b; do ...; done`
会报 syntax error，`/dev/tcp/...` 也不存在。需要循环或测端口时，
从容器侧执行，或改用 `adb shell` 多次调用。

---

## 7. 已知限制

诚实列出当前未解决或未验证的项，便于平台评估。

| 项 | 状态 |
|---|---|
| SHM（memfd）零拷贝 | 驱动侧**默认关闭**，需 `DMD_WANT_SHM=1`。这条路驱动侧从未真正启用过（只有单元测试走过：150 帧、逐字节一致、无 fd 泄漏），真实消费者环境下未验证；浏览器沙箱能否收 `SCM_RIGHTS` 亦未实测（`vaapi-driver/src/decode.c` 的 SHM 默认关闭说明（搜 `DMD_WANT_SHM`）） |
| ~~**SELinux allow 规则缺失**~~ | **v0.3.1 已解决**。缺的是给**服务端域**的 `binder transfer`（判定按 sender，`droidspacesd` 自身 permissive 救不了），不是给 `droidspacesd` 的规则。平台已在两个策略载体加上并端到端逐字节验证通过 —— 详见 §2.2 |
| 浏览器沙箱收 `SCM_RIGHTS` | **未验证**（Firefox RDD / Chrome GPU 进程有 seccomp 过滤） |
| Firefox 的 `MOZ_DISABLE_RDD_SANDBOX=1` | 该环境变量的原始理由是"RDD 沙箱禁止驱动创建 **TCP** socket"。**改用 Unix socket 后是否仍需要它，尚未实测**。它降低 RDD 进程隔离强度，属安全权衡 |
| socket 文件权限 | 当前 `chmod 0666`，任何容器内进程可连；应收紧到特定 gid，等平台确认目标 gid |
| 鉴权 | Unix socket 路径靠文件权限，但在 0666 下不构成隔离；TCP 路径完全无鉴权且 loopback 在 host 型容器不构成边界。**不要在多租户或不可信 App 环境下使用当前版本** |
| 单文件挂载场景 | 若平台挂的是单个 socket 文件而非目录，daemon 每次重启后**必须重新挂载**（inode 变了） |
| 残留 socket 清理 | daemon 启动时若发现无人监听的残留 socket 文件会 `unlink` 重建，这会**换掉 inode**，已有 bind mount 失效（`src/decode-daemon.c` 的 flock 判活（搜 `LOCK_EX`）） |
| 并发上限 | `MAX_CLIENTS = 8`，硬件支持 16，**尚未测到上限行为**；超限时直接关闭新连接，客户端只看到连接被断，没有明确拒绝原因 |
| 编解码覆盖 | H.264 / HEVC / VP9 / VP8 已真机端到端逐字节验证。**未验证**：高位深与非 4:2:0（HEVC Main10、VP9 Profile2、H.264 High 10 / 4:2:2）——驱动不声明这些能力 |
| HEVC 例外 | SPS 里 `num_short_term_ref_pic_sets > 0` 的码流无法重建参数集，驱动回 `VA_STATUS_ERROR_UNIMPLEMENTED` 让上层干净回落软解 |
| 分辨率上升越界 | 流内分辨率涨到超过握手声明的上限时，SHM 会话会被终止（TCP 模式无此限制）；解码器内部重配路径**未验证** |
| seek | 驱动尚未实现会话重建式 seek（当前没有消费者在同一 context 上 seek）；ffmpeg 层面的 seek 已验证一致 |
| 平台托管细节 | daemon 的放置位置、启动时机、重启与日志收集策略均**待平台确认**，本项目不自带进程管理 |

---

## 相关文档

- [项目总览](../README.md)
- [平台实测事实](verified-platform-facts.md)
- [VA-API Proxy 驱动（容器侧）](../vaapi-driver/README.md)
- [性能实测与优化路线](performance-and-roadmap.md)
