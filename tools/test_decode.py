#!/usr/bin/env python3
"""
最小参考实现 —— 用来验证 decode-daemon 的线路协议（v2）。

不依赖 ffmpeg，只做三件事：切分输入、按协议收发、统计结果。
生产用途请使用 client/ 下的 decode-client。

用法:
    python3 test_decode.py <端口> <文件> [codec] [tcp|shm]

    codec: h264(默认) | hevc | vp9 | vp8
           h264/hevc  读 Annex B 裸流，按起始码切成 NALU（必须带起始码）
           vp9/vp8    读 IVF 容器，每个 packet 是一整帧（不能带起始码）

    tcp(默认) 帧数据经 socket 传回
    shm       帧数据放共享内存，socket 只传槽位号

示例:
    python3 test_decode.py 20003 test1080.h264
    python3 test_decode.py 20003 test720.vp9.ivf vp9
    python3 test_decode.py 20003 test1080.h264 h264 shm
"""
import array
import mmap
import os
import socket
import struct
import select
import sys

HELLO_MAGIC = 0x444D4400
HELLO_VERSION = 2
FMTDESC_SENTINEL = 0xFFFFFFFF
SHMFRAME_SENTINEL = 0xFFFFFFFE
FMTDESC_BYTES = 32
SHM_CTRL_BYTES = 4096

XFER_TCP = 0
XFER_SHM = 1

CODEC_IDS = {"h264": 0, "hevc": 1, "vp9": 2, "vp8": 3}

# daemon 的握手拒绝状态码
REJECT_REASONS = {
    1: "协议版本不支持",
    2: "编解码器不支持",
    3: "分辨率超出硬件范围",
    4: "缺少握手",
}


def split_annexb(data):
    """按 Annex B 起始码切分，返回带起始码的 NALU 列表。

    daemon 依赖起始码定位 nal_unit_header 来识别参数集，所以必须保留。
    """
    starts = []
    i = 0
    n = len(data)
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append(i)
                i += 3
                continue
            if i + 3 < n and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append(i)
                i += 4
                continue
        i += 1
    return [data[starts[k]:(starts[k + 1] if k + 1 < len(starts) else n)]
            for k in range(len(starts))]


def split_ivf(data):
    """解析 IVF 容器，返回帧列表（VP8/VP9 不能带起始码）。"""
    if data[:4] != b"DKIF":
        raise SystemExit("不是 IVF 文件（VP8/VP9 需要 IVF 容器）")
    hdr_len = struct.unpack("<H", data[6:8])[0]
    frames = []
    pos = hdr_len
    while pos + 12 <= len(data):
        size = struct.unpack("<I", data[pos:pos + 4])[0]
        pos += 12                      # 4B 长度 + 8B 时间戳
        if pos + size > len(data):
            break
        frames.append(data[pos:pos + size])
        pos += size
    return frames


def guess_size(units, codec):
    """从码流里粗略取出分辨率，仅用于握手声明。

    daemon 会在 FORMAT_CHANGED 时回传真实尺寸，所以这里不准也不影响正确性。
    IVF 头里带有准确尺寸；Annex B 需要解析 SPS，这里不做，直接用 1080p 兜底。
    """
    if codec in ("vp9", "vp8"):
        return None                    # 调用方从 IVF 头读
    return (1920, 1080)


def shm_attach(name):
    """连到 daemon 的 abstract socket，用 SCM_RIGHTS 领取 memfd 并 mmap。

    名字由 daemon 决定并通过握手响应传来 —— 客户端不知道自己是第几个连接，
    猜名字必然串台。abstract socket 属于 net namespace，容器与 Android 共享它；
    而 mount namespace 是隔离的，所以路径形式的 Unix socket 不可用。
    """
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.settimeout(10)
    c.connect("\0" + name)                 # 前导 NUL = abstract namespace
    msg, anc, _flags, _addr = c.recvmsg(12, socket.CMSG_LEN(4))
    fds = array.array("i")
    for level, typ, data in anc:
        if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
            fds.frombytes(data[:len(data) - (len(data) % 4)])
    c.close()
    if not fds:
        raise SystemExit("未收到 memfd")
    slots, slot_size, total = struct.unpack(">III", msg)
    mm = mmap.mmap(fds[0], total, mmap.MAP_SHARED,
                   mmap.PROT_READ | mmap.PROT_WRITE)
    os.close(fds[0])                       # mmap 之后 fd 不再需要
    return mm, slots, slot_size


class Receiver:
    """按协议解析 daemon 的回传：哨兵帧头引出控制消息，否则是帧数据。"""

    def __init__(self, sock, shm=None, slots=0, slot_size=0):
        self.sock = sock
        self.buf = bytearray()
        self.frames = 0
        self.fmt = None
        self.fmt_changes = 0
        self.last_dims = None
        self.shm = shm
        self.slots = slots
        self.slot_size = slot_size
        self.first_bytes = None

    def _release(self, slot):
        """归还槽位。不归还会让 daemon 等约 1 秒后判定客户端卡死。"""
        off = slot * 4
        self.shm[off:off + 4] = struct.pack("<I", 0)

    def _parse(self):
        while len(self.buf) >= 12:
            w, h, size = struct.unpack(">III", self.buf[:12])

            if size == SHMFRAME_SENTINEL:
                if len(self.buf) < 20:
                    return
                slot, dlen = struct.unpack(">II", bytes(self.buf[12:20]))
                del self.buf[:20]
                if self.shm is None or not (0 <= slot < self.slots):
                    raise SystemExit("非法共享内存槽位: %d" % slot)
                base = SHM_CTRL_BYTES + slot * self.slot_size
                if self.first_bytes is None:
                    self.first_bytes = bytes(self.shm[base:base + 32])
                self.frames += 1
                self.last_dims = (w, h)
                self._release(slot)
                continue

            if size == FMTDESC_SENTINEL:
                if len(self.buf) < 12 + FMTDESC_BYTES:
                    return
                body = struct.unpack(">8I", bytes(self.buf[12:12 + FMTDESC_BYTES]))
                del self.buf[:12 + FMTDESC_BYTES]
                prev = self.fmt
                self.fmt = body
                self.fmt_changes += 1
                if prev and (prev[0], prev[1]) != (body[0], body[1]):
                    print("  流内分辨率变更: %dx%d -> %dx%d"
                          % (prev[0], prev[1], body[0], body[1]))
                continue

            if not (0 < size <= 8 * 1024 * 1024):
                raise SystemExit("帧大小异常: %d（协议不同步）" % size)
            if len(self.buf) < 12 + size:
                return
            del self.buf[:12 + size]
            self.frames += 1
            self.last_dims = (w, h)

    def pump(self, block=False, timeout=5.0):
        """读取可用数据。block=True 时等到连接关闭。"""
        while True:
            if not block:
                ready, _, _ = select.select([self.sock], [], [], 0)
                if not ready:
                    return True
            else:
                ready, _, _ = select.select([self.sock], [], [], timeout)
                if not ready:
                    return False
            chunk = self.sock.recv(1 << 22)
            if not chunk:
                return False
            self.buf += chunk
            self._parse()
            if not block:
                return True


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    port = int(sys.argv[1])
    path = sys.argv[2]
    codec = sys.argv[3].lower() if len(sys.argv) > 3 else "h264"
    xfer_arg = sys.argv[4].lower() if len(sys.argv) > 4 else "tcp"

    if codec not in CODEC_IDS:
        print("未知 codec: %s（支持 %s）" % (codec, "/".join(CODEC_IDS)))
        return 1
    if xfer_arg not in ("tcp", "shm"):
        print("未知传输模式: %s（支持 tcp/shm）" % xfer_arg)
        return 1
    want_xfer = XFER_SHM if xfer_arg == "shm" else XFER_TCP

    data = open(path, "rb").read()

    if codec in ("vp9", "vp8"):
        units = split_ivf(data)
        width, height = struct.unpack("<HH", data[12:16])
        annexb = False
    else:
        units = split_annexb(data)
        width, height = guess_size(units, codec)
        annexb = True

    if not units:
        print("没有解析出任何数据单元")
        return 1

    print("输入: %s" % path)
    print("  编码 %s, 声明尺寸 %dx%d, %d 个%s"
          % (codec.upper(), width, height, len(units),
             "NALU" if annexb else "帧"))

    sock = socket.socket()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(15)
    sock.connect(("127.0.0.1", port))

    def read_exact(n):
        got = b""
        while len(got) < n:
            chunk = sock.recv(n - len(got))
            if not chunk:
                raise SystemExit("握手响应中断")
            got += chunk
        return got

    # 握手是必需的：daemon 靠它确定 mime、初始尺寸与传输模式
    sock.sendall(struct.pack(">IIIIII", HELLO_MAGIC, HELLO_VERSION,
                             CODEC_IDS[codec], width, height, want_xfer))

    # 响应是变长的: [status][实际模式][名字长度][名字...]
    status, mode, nlen = struct.unpack(">III", read_exact(12))
    shm_name = read_exact(nlen).decode() if nlen else ""
    if status != 0:
        print("握手被拒绝: %s (status=%d)"
              % (REJECT_REASONS.get(status, "未知原因"), status))
        return 1

    # daemon 可能把 SHM 降级为 TCP，必须看它实际给的模式而不是自己请求的
    shm = None
    slots = slot_size = 0
    if mode == XFER_SHM:
        shm, slots, slot_size = shm_attach(shm_name)
        print("  传输 SHM: %d 槽 x %d 字节 (%s)" % (slots, slot_size, shm_name))
    else:
        print("  传输 TCP%s"
              % ("（请求了 SHM 但被降级）" if want_xfer == XFER_SHM else ""))

    rx = Receiver(sock, shm, slots, slot_size)

    # 必须边发边收：daemon 单会话内串行处理，只发不收会让它的写阻塞在
    # 满的 socket 缓冲上，双方僵死并丢失全部解码帧。
    for unit in units:
        sock.sendall(struct.pack(">I", len(unit)) + unit)
        if not rx.pump():
            break

    # 关闭写端触发 flush，否则解码器里排队的帧取不出来
    sock.shutdown(socket.SHUT_WR)
    while rx.pump(block=True):
        pass
    sock.close()

    if rx.fmt:
        bw, bh, stride, slice_h, cl, ct, cr, cb = rx.fmt
        print("  最终格式: 缓冲 %dx%d, stride=%d, slice_height=%d" % (bw, bh, stride, slice_h))
        print("            显示区域 %dx%d (crop %d,%d - %d,%d)"
              % (cr - cl + 1, cb - ct + 1, cl, ct, cr, cb))
        if rx.fmt_changes > 1:
            print("  格式变更次数: %d" % rx.fmt_changes)

    if shm is not None:
        shm.close()

    print("RESULT: %d frames decoded from %s" % (rx.frames, path))
    return 0 if rx.frames > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
