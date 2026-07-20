#!/usr/bin/env python3
"""心跳机制测试：PING/PONG 响应 + 超时踢下线"""
import socket
import json
import time

HOST = "127.0.0.1"
PORT = 6000

def send(sock, data):
    sock.sendall((json.dumps(data) + "\n").encode())

def recv(sock, timeout=3):
    sock.settimeout(timeout)
    try:
        return sock.recv(4096).decode().strip()
    except socket.timeout:
        return None

def test_ping_pong():
    print("=== 测试 1：PING → PONG ===")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))

    send(sock, {"msgid": 1, "name": "test_hb", "password": "123456"})
    resp = recv(sock)
    print(f"  登录响应: {resp}")

    send(sock, {"msgid": 14})
    resp = recv(sock, timeout=2)
    print(f"  PING 响应: {resp}")

    try:
        r = json.loads(resp) if resp else {}
    except:
        r = {}
    ok = r.get("msgid") == 15
    print(f"  {'✅ PING/PONG 正常' if ok else '❌ 期望 msgid=15, 实际=' + str(r.get('msgid'))}\n")
    sock.close()

def test_keepalive():
    print("=== 测试 2：持续心跳保活（~32s）===")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((HOST, PORT))

    send(sock, {"msgid": 1, "name": "test_hb2", "password": "123456"})
    print(f"  登录响应: {recv(sock)}")

    for i in range(4):
        time.sleep(8)
        send(sock, {"msgid": 14})
        resp = recv(sock, timeout=2)
        if resp:
            print(f"  第{i+1}次心跳: {resp.strip()}")
        else:
            print(f"  第{i+1}次心跳: 无响应（已被踢）")
            sock.close()
            return
    print("  ✅ 32s 内持续在线\n")
    sock.close()

if __name__ == "__main__":
    test_ping_pong()
    test_keepalive()
    print("=== 全部测试完成 ===")
