import socket, struct, select, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19876
FILE = sys.argv[2] if len(sys.argv) > 2 else "/tmp/real_test.h264"

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.connect(("127.0.0.1", PORT))
print("Connected to port %d" % PORT)

with open(FILE, "rb") as f:
    data = f.read()

# Send all NALUs
pos = 0; count = 0
while pos < len(data) - 3 and count < 30:
    if data[pos] == 0 and data[pos+1] == 0 and data[pos+2] == 1:
        start = pos; pos += 3
        while pos < len(data) - 3:
            if data[pos] == 0 and data[pos+1] == 0 and data[pos+2] == 1: break
            pos += 1
        s.sendall(struct.pack(">I", pos - start) + data[start:pos])
        count += 1
    else:
        pos += 1
print("Sent %d NALUs" % count)
s.shutdown(socket.SHUT_WR)

# Read frames
frames = 0
for i in range(20):
    ready = select.select([s], [], [], 5)
    if ready[0]:
        hdr = s.recv(12)
        if len(hdr) == 12:
            w, h, fs = struct.unpack(">III", hdr)
            if 0 < fs < 20000000:
                frame = b""
                while len(frame) < fs:
                    chunk = s.recv(min(65536, fs - len(frame)))
                    if not chunk: break
                    frame += chunk
                frames += 1
                print("Frame %d: %dx%d %d bytes" % (frames, w, h, fs))
            else:
                break
        elif len(hdr) == 0:
            break
    else:
        break

print("RESULT: %d frames decoded from %s" % (frames, FILE))
s.close()
