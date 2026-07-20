#!/usr/bin/env python3
"""测试超时踢下线：登录后不发心跳，等 90s 看是否被断开"""
import socket
import json
import time

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("127.0.0.1", 6000))

# 登录
sock.sendall((json.dumps({"msgid":1,"name":"test_hb","password":"123456"}) + "\n").encode())
print("登录:", sock.recv(4096).decode().strip())

# 不发任何心跳，静等
print("静等 95s，不发心跳...")
time.sleep(95)

# 尝试收数据，看连接是否被服务端断开
sock.settimeout(3)
try:
    data = sock.recv(4096)
    if data:
        print("收到:", data.decode().strip())
    else:
        print("✅ 连接已被服务端关闭（recv 返回空）—— 超时踢下线成功")
except (ConnectionResetError, BrokenPipeError, OSError) as e:
    print(f"✅ 连接已被服务端关闭 ({type(e).__name__})—— 超时踢下线成功")
except socket.timeout:
    print("⚠️ 95s 后仍能 recv（超时未踢？）")

sock.close()
