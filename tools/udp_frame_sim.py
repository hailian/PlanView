#!/usr/bin/env python3
# UDP 帧数据源模拟器：向目标地址周期性发送 TLV 帧，供 PageViewer 帧数据源联调。
#
# 帧格式（TLV，大端）：T(1字节槽位) L(2字节负载长) V(负载)
#   槽位 1  u16 温度（中值 150，幅值 120，周期 8s）   配合标签 scale=0.1
#   槽位 2  u8  泵运行（1Hz 方波 0/1）
#   槽位 3  f32 液位（随机游走 0..40）
#
# 配套工程配置（设计器 工具→数据源配置）：
#   启用帧数据源 + UDP + 本地绑定端口 9001 + 目标 127.0.0.1:9001
#   TLV：T 字节数 1，L 字节数 2，大端，L 不含帧头
#   TLV 模式字段按槽位(T)匹配、偏移相对负载 V：
#     温度 槽位1 offset=0 类型 u16 大端 地址1；泵 槽位2 offset=0 类型 u8 地址2；液位 槽位3 offset=0 类型 f32 大端 地址3
#
# 用法: python udp_frame_sim.py [目标端口]   (默认 9001)

import math
import random
import socket
import sys
import time


def tlv(slot: int, payload: bytes) -> bytes:
    return bytes([slot]) + len(payload).to_bytes(2, "big") + payload


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
    addr = ("127.0.0.1", port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    t0 = time.time()
    walk = 20.0
    print(f"[sim] UDP 帧模拟器 -> {addr[0]}:{addr[1]}  (Ctrl+C 退出)")
    while True:
        t = time.time() - t0
        temp = round(150 + 120 * math.sin(2 * math.pi * t / 8.0))
        pump = int(t) % 2
        walk = max(0.0, min(40.0, walk + random.uniform(-0.5, 0.5)))

        sock.sendto(tlv(1, temp.to_bytes(2, "big")), addr)
        sock.sendto(tlv(2, bytes([pump])), addr)
        sock.sendto(tlv(3, bytearray(__import__("struct").pack(">f", walk))), addr)
        time.sleep(0.2)


if __name__ == "__main__":
    main()
