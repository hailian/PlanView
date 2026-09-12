#!/usr/bin/env python3
# SoftG TCP 数据服务器模拟器（联调用）
#
# 协议（行文本，UTF-8，\n 结尾）:
#   PING\n                        -> PONG\n
#   READ <n> <idx1> <idx2> ...\n   -> VALUES <n> <idx>:<type>:<value> ...\n
#   WRITE <idx> <type> <value>\n   -> OK <idx>\n | ERR <message>\n
#   type ∈ bool|int16|uint16|int32|uint32|float32
#
# 内置演示数据:
#   槽位 0..5  float32 正弦波（周期 8s，中值 15+i*5，幅值 12，保证为正且 Temp1 可越过告警阈值 25）
#   槽位 6     float32 缓变随机游走
#   槽位 7     bool   1Hz 方波
#   槽位 8     uint16 计数器
# 其余槽位可由 WRITE 写入后持久读出。
#
# 用法: python tcp_sim.py [port]     (默认 9000)

import math
import random
import socket
import socketserver
import sys
import threading
import time

TYPES = {"bool", "int16", "uint16", "int32", "uint32", "float32"}

lock = threading.Lock()
slots = {}  # idx -> (type, value)
writes_log = []


def init_demo_slots():
    for i in range(6):
        slots[i] = ("float32", 0.0)
    slots[6] = ("float32", 20.0)
    slots[7] = ("bool", False)
    slots[8] = ("uint16", 0)


def animate():
    t0 = time.time()
    walk = 20.0
    while True:
        t = time.time() - t0
        with lock:
            for i in range(6):
                mid = 15.0 + i * 5.0
                phase = i * 0.7
                slots[i] = ("float32",
                            round(mid + 12.0 * math.sin(2 * math.pi * t / 8.0 + phase), 4))
            walk += random.uniform(-0.5, 0.5)
            walk = max(0.0, min(40.0, walk))
            slots[6] = ("float32", round(walk, 3))
            slots[7] = ("bool", int(t) % 2 == 0)
            slots[8] = ("uint16", int(t) % 100000)
        time.sleep(0.05)


def fmt_value(v):
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, float):
        return f"{v:.9g}"
    return str(v)


def handle_line(line):
    if line == "PING":
        return "PONG"
    if line.startswith("READ "):
        toks = line[5:].split()
        idxs = toks[1:] if len(toks) >= 1 and toks[0].isdigit() else toks  # 首词为数量，跳过
        parts = []
        for tok in idxs:
            try:
                idx = int(tok)
            except ValueError:
                parts.append(f"{tok}:?:-")
                continue
            with lock:
                slot = slots.get(idx)
            if slot is None:
                parts.append(f"{idx}:?:-")
            else:
                t, v = slot
                parts.append(f"{idx}:{t}:{fmt_value(v)}")
        return "VALUES " + str(len(parts)) + " " + " ".join(parts)
    if line.startswith("WRITE "):
        try:
            _, idx_s, typ, val_s = line.split(" ", 3)
            idx = int(idx_s)
            if typ not in TYPES:
                return f"ERR unknown type {typ}"
            if typ == "bool":
                v = val_s in ("1", "true")
            elif typ == "float32":
                v = float(val_s)
            else:
                v = int(val_s)
        except ValueError as e:
            return f"ERR {e}"
        with lock:
            slots[idx] = (typ, v)
            writes_log.append(f"{time.strftime('%H:%M:%S')} WRITE #{idx} <- {typ} {val_s}")
        print(f"[sim] 写入槽位 {idx}: {typ} = {val_s}")
        return f"OK {idx}"
    return "ERR unknown command"


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        peer = self.client_address
        print(f"[sim] 客户端接入 {peer}")
        try:
            for raw in self.rfile:  # 行迭代（二进制缓冲）
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if not line:
                    continue
                resp = handle_line(line)
                self.wfile.write((resp + "\n").encode("utf-8"))
                print(f"[sim] {peer} '{line}' -> '{resp}'")
        except (ConnectionAbortedError, ConnectionResetError, OSError):
            pass
        print(f"[sim] 客户端断开 {peer}")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    init_demo_slots()
    threading.Thread(target=animate, daemon=True).start()
    with Server(("0.0.0.0", port), Handler) as srv:
        print(f"[sim] SoftG TCP 数据服务器已启动 0.0.0.0:{port}")
        print("[sim] 演示槽位: 0-5 正弦波(float32) 6 随机游走(float32) 7 方波(bool) 8 计数器(uint16)")
        print("[sim] Ctrl+C 退出")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
