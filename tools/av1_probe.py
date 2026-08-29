#!/usr/bin/env python3
"""
AV1 直送探针 —— 绕过 VA-API 驱动，把 OBU temporal unit 直接喂给 decode-daemon。

目的：判定"MediaCodec 不出帧"是驱动合成的问题，还是 daemon/解码器侧的问题。
做法：从裸 .obu 文件按 temporal delimiter 切分成 temporal unit，逐个按线路协议
发给 daemon，统计回传帧数。

用法:
    python3 av1_probe.py <sock路径|端口> <file.obu> [单元数] [tcp|shm]

判定：
    送原始码流也 0 帧  → 问题在 daemon / MediaCodec 侧，与合成无关
    送原始码流能出帧  → 问题在驱动合成或送料时序
"""
import os
import socket
import struct
import sys

HELLO_MAGIC = 0x444D4400
HELLO_VERSION = 3
FMTDESC_SENTINEL = 0xFFFFFFFF
SHMFRAME_SENTINEL = 0xFFFFFFFE
CODEC_H264 = 0
CODEC_AV1 = 4
XFER_TCP, XFER_SHM = 0, 1


def split_temporal_units(data):
    """按 OBU_TEMPORAL_DELIMITER(type=2) 切分。每个 TU 以 TD 开头。"""
    obus = []
    i = 0
    while i < len(data):
        h = data[i]
        obu_type = (h >> 3) & 0xF
        has_size = (h >> 1) & 1
        j = i + 1
        if (h >> 2) & 1:          # extension flag
            j += 1
        size = 0
        if has_size:
            shift = 0
            while True:
                b = data[j]
                size |= (b & 0x7F) << shift
                j += 1
                shift += 7
                if not (b & 0x80):
                    break
        else:
            size = len(data) - j
        obus.append((obu_type, i, j + size - i))
        i = j + size

    units, cur = [], None
    for t, off, ln in obus:
        if t == 2:                # temporal delimiter starts a new TU
            if cur is not None:
                units.append(cur)
            cur = [off, ln]
        elif cur is not None:
            cur[1] += ln
        else:
            cur = [off, ln]
    if cur is not None:
        units.append(cur)
    return [data[o:o + l] for o, l in units]


def split_annexb(data):
    """按 Annex-B 起始码切成 NALU（每个单元自带起始码），用于 H264 对照测试。"""
    starts = []
    i = 0
    while i < len(data) - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append(i)
                i += 3
                continue
            if i < len(data) - 4 and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append(i)
                i += 4
                continue
        i += 1
    return [data[starts[k]:(starts[k + 1] if k + 1 < len(starts) else len(data))]
            for k in range(len(starts))]


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    endpoint = sys.argv[1]
    path = sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    xfer = XFER_SHM if (len(sys.argv) > 4 and sys.argv[4] == "shm") else XFER_TCP

    data = open(path, "rb").read()
    # codec 由文件扩展名决定：.obu → AV1（按 TD 切分），其它 → H264（按起始码切分）
    is_av1 = path.endswith(".obu")
    codec = CODEC_AV1 if is_av1 else CODEC_H264
    units = split_temporal_units(data) if is_av1 else split_annexb(data)
    # 过滤掉过短的单元（纯 TD 或 show_existing_frame，不含实际 tile 数据）
    # —— 它们不该影响解码，但在排查阶段先排除干扰变量。
    if os.environ.get("PROBE_MIN_LEN"):
        m = int(os.environ["PROBE_MIN_LEN"])
        units = [u for u in units if len(u) >= m]
    if limit:
        units = units[:limit]
    print("codec=%s 切出 %d 个单元，前 3 个长度: %s"
          % ("av1" if is_av1 else "h264", len(units), [len(u) for u in units[:3]]))

    if endpoint.isdigit():
        sock = socket.create_connection(("127.0.0.1", int(endpoint)), timeout=10)
    else:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(endpoint)

    # 握手：magic + version + codec + width + height + xfer（全大端）
    sock.sendall(struct.pack(">6I", HELLO_MAGIC, HELLO_VERSION,
                             codec, 1920, 1080, xfer))
    resp = recv_exact(sock, 12)
    if not resp:
        print("握手无响应")
        return 1
    status, acc_xfer, _ = struct.unpack(">3I", resp)
    print("握手响应: status=%u xfer=%u" % (status, acc_xfer))
    if status != 0:
        print("daemon 拒绝握手")
        return 1
    if HELLO_VERSION >= 3:
        recv_exact(sock, 16)          # endpoint dev/ino 扩展

    if acc_xfer == XFER_SHM:
        print("daemon 选了 SHM，本探针只支持 TCP —— 请用 tcp 模式")
        return 1

    frames = 0
    if os.environ.get("PROBE_BURST"):
        # 突发模式：一次性把所有单元灌进去，再统一收帧。
        # 用途：解码器声明 output delay N 时，逐单元等帧会死锁
        # （它要收满 N 个输入才吐首帧，而我们在第 1 个就阻塞等待）。
        for unit in units:
            sock.sendall(struct.pack(">I", len(unit)) + unit)
        print("  已突发送入 %d 个单元，开始收帧" % len(units))
        sock.settimeout(8)
        try:
            while True:
                hdr = recv_exact(sock, 12)
                if not hdr:
                    break
                w, h, sz = struct.unpack(">3I", hdr)
                if sz == FMTDESC_SENTINEL:
                    recv_exact(sock, 32)
                    continue
                if sz == SHMFRAME_SENTINEL:
                    continue
                if recv_exact(sock, sz) is None:
                    break
                frames += 1
                print("  帧 %d: %ux%u %u 字节" % (frames, w, h, sz))
        except (socket.timeout, EOFError, OSError) as e:
            print("  收帧结束: %s" % type(e).__name__)
        sock.close()
        print("结论: 突发送入 %d 单元，回传 %d 帧" % (len(units), frames))
        return 0

    for idx, unit in enumerate(units):
        sock.sendall(struct.pack(">I", len(unit)) + unit)
        sock.settimeout(3)
        try:
            while True:
                hdr = recv_exact(sock, 12)
                if not hdr:
                    raise EOFError
                w, h, sz = struct.unpack(">3I", hdr)
                if sz == FMTDESC_SENTINEL:
                    recv_exact(sock, 32)
                    print("  收到格式描述块")
                    continue
                if sz == SHMFRAME_SENTINEL:
                    print("  收到 SHM 帧通知（未预期）")
                    continue
                payload = recv_exact(sock, sz)
                if payload is None:
                    raise EOFError
                frames += 1
                print("  帧 %d: %ux%u %u 字节" % (frames, w, h, sz))
                break
        except (socket.timeout, EOFError):
            pass
        except (ConnectionResetError, OSError) as e:
            print("  第 %d 个单元后连接断开: %s" % (idx + 1, e))
            break
        if idx == 0:
            print("  第 1 个单元送完，累计回帧 %d" % frames)

    # 送排空请求（长度 0）逼出剩余帧
    try:
        sock.sendall(struct.pack(">I", 0))
        sock.settimeout(5)
        while True:
            hdr = recv_exact(sock, 12)
            if not hdr:
                break
            w, h, sz = struct.unpack(">3I", hdr)
            if sz in (FMTDESC_SENTINEL, SHMFRAME_SENTINEL):
                recv_exact(sock, 32 if sz == FMTDESC_SENTINEL else 0)
                continue
            if recv_exact(sock, sz) is None:
                break
            frames += 1
            print("  排空帧 %d: %ux%u" % (frames, w, h))
    except (socket.timeout, EOFError, OSError):
        pass

    sock.close()
    print("结论: 送入 %d 单元，回传 %d 帧" % (len(units), frames))
    return 0


if __name__ == "__main__":
    sys.exit(main())
