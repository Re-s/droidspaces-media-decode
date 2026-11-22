# Firefox RDD 与直接 V4L2

此入口仅用于已验证的原生 **Firefox 160、GNU/Linux AArch64、GNU libc** 环境，
默认可执行文件为 `/usr/bin/firefox`。专用兼容库
`/usr/local/lib/droidspaces-firefox/libdmd-firefox-compat.so` 在 RDD 启动阶段预开
system DMA heap、KGSL 和 render 设备的缓存 fd；驱动通过配套 weak API 复用
heap fd。本机已经实际完成 H.264 硬件解码 60/60 帧。库和驱动必须使用配套版本，
结果不扩展到其他 Firefox 版本、架构、libc 或设备。不需要 Android daemon。

兼容库为 RDD 增加了**显式、有限的 KGSL ioctl 策略**，不是原封不动的默认
沙箱：只放行白名单完整 ioctl request 与固定缓存设备 fd 的组合，其他 ioctl
仍由原策略决定。内核过滤器禁止关闭或覆盖缓存 fd，并主动拒绝 execve、execveat
及三个 io_uring syscall；非 AArch64 syscall 架构会终止进程。其余 syscall
继续走原策略。RDD、内容和 GPU 的 Mozilla 沙箱均未关闭，但 RDD 获得了新增
设备访问能力；这是此设备兼容方案的明确策略变化。

此方案要求锁定 `dom.ipc.forkserver.enable=false`，使 RDD 通过独立 exec 加载
专用库。该首选项只改变子进程启动方式；KGSL 权限来自上面的配套兼容库策略。
安装器只合并这一项 Firefox Preferences，不覆写用户 profile、硬解黑名单、
软件回退或其他首选项。

## 安装

先由系统安装过程部署驱动和专用库，再以 root 安装一项策略：

```sh
sh tools/configure-firefox-vaapi.sh --install-system-policy
```

默认文件为 `/usr/lib/firefox/distribution/policies.json`；其他原生安装位置可
显式指定 `--policy-file /absolute/path/distribution/policies.json`。
安装器备份原文件，保留已有 policies 与 Preferences，再原子合并：

```json
{"policies":{"Preferences":{"dom.ipc.forkserver.enable":{"Value":false,"Status":"locked"}}}}
```

接着在 Gold 等桌面普通用户的会话中运行：

```sh
sh tools/configure-firefox-vaapi.sh
sh tools/configure-firefox-vaapi.sh --verify
~/.local/bin/firefox-hwdec --dmd-check
```

用户安装会写入可执行的 `~/.local/bin/firefox-hwdec`，按原 `.desktop` 生成同名
用户 override，保留名称、图标、动作、启动参数与 URL 占位符，只替换 Firefox
的 Exec 可执行文件。已有用户 override 优先，原文件会备份；不修改系统桌面文件。
找不到厂商文件时可用 `--desktop-dir /path/to/applications` 指定目录。

启动器按进程设置 `MESA_LOADER_DRIVER_OVERRIDE=msm`、`MOZ_ENABLE_WAYLAND=1`，
导出 `DMD_FIREFOX_KGSL=1` 显式启用兼容库，将专用绝对路径库前置到
`LD_PRELOAD` 并保留已有库。不设置全局环境、
`LD_LIBRARY_PATH` 或任何 `MOZ_DISABLE_*`。若继承了关闭沙箱的环境变量，则拒绝
启动并提示移除，避免沿用旧诊断环境。其他 Firefox 二进制可用绝对路径
`DMD_FIREFOX_BIN` 指定；指向本启动器或旧启动器的递归路径会被拒绝。
`--compat-lib /absolute/path/libdmd-firefox-compat.so` 可选定不同安装前缀；运行时
保留历史变量名 `DMD_FIREFOX_HEAP_LIB` 作为兼容库路径覆盖。库路径不能含空白或
冒号。启动器检查系统架构、GNU libc 及 Firefox 主版本 160；`--dmd-check` 也会
校验版本和库是否存在，并说明策略变化。Snap/Flatpak 的额外
容器访问限制不属于此原生包入口的已验证范围。

## 验证与回滚

完全退出旧 Firefox 后再启动。`--verify` 校验安装文件和锁定策略，
`firefox-hwdec --dmd-check` 校验已验证的运行环境；这两者不替代播放测试。
检查 `about:policies` 的 active Preferences 及
`about:config` 中锁定的 forkserver=false；检查 `about:support` 的硬件解码、
WebRender 和沙箱信息。实际播放至少 60 帧已知 H.264，确认 RDD 加载专用库及
VA 驱动、持有正确 V4L2 节点并实际取回帧；仅 codec 能力或成功启动不算通过。
`DMD_VA_LOG=1 firefox-hwdec` 可保留驱动证据。此方案不承诺 Firefox 硬件编码。

普通用户撤销桌面/启动器，root 单独撤销系统首选项：

```sh
sh tools/configure-firefox-vaapi.sh --uninstall
# 以下命令以 root 运行；若安装时指定 --policy-file，卸载时使用同一路径。
sh tools/configure-firefox-vaapi.sh --uninstall-system-policy
```

策略卸载只恢复这一项先前的值，保留安装后新增的其他 policies；如果整份文件
没有其他变化，则恢复原始备份内容。用户文件被其他工具修改后会明确报错，保留
当前文件及备份，不盲目覆盖。备份留作人工核对；开始下一次全新安装前应将上次
`.dmd-firefox.bak` 归档。旧 `firefox-msm-vaapi` 启动器及其他来源的关闭沙箱设置
不由新入口删除；请停用旧启动入口并检查继承环境。

参考：[Mozilla policies.json 安装位置](https://support.mozilla.org/en-US/kb/customizing-firefox-using-policiesjson)、
[Firefox 管理员策略文档](https://firefox-admin-docs.mozilla.org/)。
