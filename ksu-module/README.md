# dmd_watchdog —— decode-daemon 看护模块

补上 Droidspaces 平台缺失的那一层：`decode-daemon` 崩溃后没有任何东西会重新
拉起它（真机实测 `kill -9` 后容器硬解一直坏到手工干预）。本模块只做看护，
与上游 `droidspaces` 模块并存、互不干扰。

## 它做什么

每 5 秒对 daemon 的 unix socket 做一次**真实探活**（connect + 完整握手）：

- daemon 没在跑 → 自动拉起，拉起后复验
- 连得上但不服务（半死）→ 先清掉再拉起
- endpoint inode 不匹配 → **只告警，不动手**（见下文"排障"第 2 条）
- 健康 → 什么都不做，状态变化时才记一行日志

## 安装

> **模块 zip 自带 `decode-daemon`**，刷入即完成**安卓侧**部署
> （装到 `/data/local/Droidspaces/dmd/bin/`，并建好 `bin/ run/ logs/`）。
> **容器侧要自己装**：把 Release 里的 `msm_drm_drv_video.so` 放进容器的
> `/usr/lib/aarch64-linux-gnu/dri/`，再确认容器内有 `libva2`、`libva-drm2`，
> 且平台已透传 `/dev/dri/renderD128`。之所以不自动装，是容器名与发行版
> 布局都不确定（Alpine 没有那条多架构路径），猜错就会静默装到无用位置。

### 方式 A：管理器刷入（推荐）

KSU / Magisk 管理器 → 模块 → 从本地安装 → 选 `dmd_watchdog-<版本>.zip` → 重启。

安装期 `customize.sh` 会打印每一步结果，其中应当看到 `decode-daemon → dmd/bin/`。

### 方式 B：手动解压（不重启即可生效）

⚠️ 这条路**绕过 `customize.sh`**，所以 daemon 与目录都得自己部署 ——
下面第二段就是补做 `customize.sh` 的活。

```sh
su
mkdir -p /data/adb/modules/dmd_watchdog
unzip -o dmd_watchdog-*.zip -d /data/adb/modules/dmd_watchdog   # 或 busybox unzip
chown -R root:root /data/adb/modules/dmd_watchdog
chmod 755 /data/adb/modules/dmd_watchdog/*.sh \
          /data/adb/modules/dmd_watchdog/dmd-probe
chcon -R u:object_r:system_file:s0 /data/adb/modules/dmd_watchdog

# 部署安卓侧运行时（管理器刷入时由 customize.sh 自动完成）
DMD=/data/local/Droidspaces/dmd
mkdir -p "$DMD/bin" "$DMD/run" "$DMD/logs"
mv -f /data/adb/modules/dmd_watchdog/decode-daemon "$DMD/bin/decode-daemon"
chmod 755 "$DMD/bin/decode-daemon"

# 手动启动看护（不必等重启）
setsid /data/adb/modules/dmd_watchdog/service.sh &
```

装完自检：

```sh
# 看护应报 healthy，且探针能拿到真实帧
cat /data/local/Droidspaces/dmd/run/watchdog.state
/data/adb/modules/dmd_watchdog/dmd-probe /data/local/Droidspaces/dmd/run/decode.sock
# 期望输出形如：probe: 健康 帧=5 dev=... ino=...
```

> `unzip` 设备上可能没有：`busybox unzip` 同样可用（KernelSU 自带 busybox）。

## 首次验证

装完等 1 分钟，在管理器里点模块的「执行」（跑的是 `action.sh`），或手工执行：

```sh
su
/data/adb/modules/dmd_watchdog/action.sh
```

健康时看到：

```
看护进程: 运行中 (PID xxxx)
状态标记: healthy
probe: 健康 dev=64801 ino=1343220
判定: 健康
```

终极验证（可选）：`kill -9 $(pidof decode-daemon)` 杀掉 daemon，10 秒内看护应
自动补回并在日志里记 `重启成功并复验通过`。容器侧硬解应当无感恢复
（前提：容器用的是目录级挂载，见排障第 2 条）。

## 配置

配置文件 `/data/local/Droidspaces/.dmd-watchdog.conf`（不存在则全用默认值）。
开机流程里设不了环境变量，改这个文件才是正道；内容就是普通 shell 赋值：

```sh
# /data/local/Droidspaces/.dmd-watchdog.conf
INTERVAL=10        # 探活间隔（秒）。默认 5。每次探活是一次 connect+握手
MAX_FAILS=3        # 连续重启失败上限。默认 5，达到后进冷却
COOLDOWN=600       # 冷却时长（秒）。默认 300。冷却后重试，不彻底放弃
DMD_WD_GRACE=90    # 开机后让位给平台的秒数。默认 45（service.sh 读）
DMD_WD_LOG_MAX=262144  # 日志轮转阈值字节。默认 256KB
```

改完重启看护生效：

```sh
kill $(cat /data/local/Droidspaces/.dmd-watchdog.pid)
setsid /data/adb/modules/dmd_watchdog/service.sh &
```

## 文件与路径

| 路径 | 说明 |
|---|---|
| `/data/adb/modules/dmd_watchdog/` | 模块本体 |
| `/data/local/Droidspaces/.dmd-watchdog.pid` | 看护 PID |
| `/data/local/Droidspaces/.dmd-watchdog.state` | 当前状态一行字 |
| `/data/local/Droidspaces/.dmd-watchdog.conf` | 你的配置（可选） |
| `/data/local/Droidspaces/Logs/dmd-watchdog.log` | 看护日志（自动轮转 `.1`） |
| `/data/local/Droidspaces/Logs/decode-daemon.log` | 看护拉起的 daemon 的输出 |

## 排障

### 1. daemon 反复被杀又拉起（日志见多次"已拉起"）

daemon 本体在崩溃循环。看 `/data/local/Droidspaces/Logs/decode-daemon.log`
找根因。常见原因历史上都见过：SELinux 规则缺失导致建 codec 时 SIGABRT；
socket 目录不可写。看护的上限保护会在连续 5 次失败后进 300s 冷却，
不会无限刷进程。

### 2. 日志出现 "endpoint inode 不匹配"，看护不动手 —— 这是设计行为

含义：探活连到的端点不是本机 daemon 的监听端点。典型成因是平台把单个
socket **文件**而非**目录** bind mount 进容器：daemon 重启换 inode 后，
容器侧挂载钉着已死的 inode。此时重启 daemon 救不了容器——只会再换个 inode。

判定方法（宿主）：

```sh
stat -c %i /data/local/Droidspaces/Decode/decode.sock     # 宿主真实 inode
# 容器里：
stat -c %i /run/dmd/decode.sock 2>/dev/null || stat -c %i /tmp/.decode-socket
# 两者不一致 = 命中本条
```

处置：换用带**目录级挂载**的平台构建（容器内挂载点为 `/run/dmd` 目录），
或重建容器让挂载重新绑定当前 socket。看护对此只告警不插手——假装修好
比明确告警更糟。

### 3. 容器硬解坏了但看护显示 healthy

看护只保证**宿主 daemon 存活且能握手**。容器侧还有驱动、挂载两个环节。
按顺序查：容器内 `vainfo` 是否报出 DroidSpaces 驱动；上面第 2 条的 inode
对账；容器内 `DMD_ENDPOINT` 指向哪。

### 4. 与上游 droidspaces 模块的关系

上游模块负责启动平台本体（droidspaces daemon、容器自启）；本模块只盯
decode-daemon 的存活。两者并存没有冲突；本模块的 sepolicy 刻意不重复声明
`droidspacesd` 相关类型（重复声明会让 magiskpolicy 报错甚至中断策略加载）。

## 卸载

管理器里移除模块，或：

```sh
kill $(cat /data/local/Droidspaces/.dmd-watchdog.pid) 2>/dev/null
rm -rf /data/adb/modules/dmd_watchdog
rm -f /data/local/Droidspaces/.dmd-watchdog.{pid,state,conf}
```

## 已知边界

- 原版平台（socket 文件级挂载）下，看护能救活 daemon，但容器侧仍是死引用，
  需要目录挂载版平台或重建容器。看护会明确告警而不是假装修好了。
- 看护不监控容器内驱动侧状态，只负责宿主 daemon 的存活与可服务性。
- 开机自启依赖 KSU 的 service.sh 执行时机；若你的 ROM 禁用了模块脚本，
  只能手动启动（方式 B 的最后一步）。

## 从源码构建

```sh
# dmd-probe 是静态 aarch64 二进制，无运行时依赖
aarch64-linux-musl-gcc -O2 -static -Wall -Wextra -o dmd-probe src/dmd-probe.c
# CI 里则在 arm64 debian 容器内 gcc -static 完成，产物等价
```

协议细节见仓库根 README 的「握手」一节；探活退出码语义见 `src/dmd-probe.c`
头部注释。
