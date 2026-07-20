#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════╗
║        CherryChat — 终端交互式聊天客户端                 ║
║                                                          ║
║  功能: 注册 | 登录 | 一对一聊天 | 群聊 | 好友 | 群组      ║
║  协议: JSON + TCP + 心跳保活                              ║
║  用法: python3 chat_client.py [host] [port]               ║
╚══════════════════════════════════════════════════════════╝
"""

import socket
import json
import threading
import time
import sys
import os
import hashlib
import signal

# ═══════════════════════════════════════════════════════════
# 配置
# ═══════════════════════════════════════════════════════════
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 6000
HEARTBEAT_INTERVAL = 30   # 心跳间隔（秒）
RECV_TIMEOUT = 1.0        # socket 读取超时（秒）

# ═══════════════════════════════════════════════════════════
# 消息类型（与 include/public.h 严格同步）
# ═══════════════════════════════════════════════════════════
(
    LOGIN_MSG,           # 1
    LOGIN_MSG_ACK,       # 2
    REG_MSG,             # 3
    REG_MSG_ACK,         # 4
    ONE_CHAT_MSG,        # 5
    ADD_FRIEND_MSG,      # 6
    CREATE_GROUP_MSG,    # 7
    ADD_GROUP_MSG,       # 8
    GROUP_CHAT_MSG,      # 9
    ADD_FRIEND_MSG_ACK,  # 10
    CREATE_GROUP_MSG_ACK,# 11
    ADD_GROUP_MSG_ACK,   # 12
    GROUP_CHAT_MSG_ACK,  # 13
    PING_MSG,            # 14
    PONG_MSG,            # 15
) = range(1, 16)

# ═══════════════════════════════════════════════════════════
# ANSI 终端颜色
# ═══════════════════════════════════════════════════════════
class C:
    R = "\033[91m"     # 红 — 错误
    G = "\033[92m"     # 绿 — 成功
    Y = "\033[93m"     # 黄 — 私聊消息
    B = "\033[94m"     # 蓝
    M = "\033[95m"     # 紫 — 群聊消息
    C = "\033[96m"     # 青 — 系统/标题
    W = "\033[97m"     # 白
    D = "\033[2m"      # 暗 — 提示
    X = "\033[0m"      # 重置
    BOLD = "\033[1m"


def _sha256(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def _clear():
    os.system("cls" if os.name == "nt" else "clear")


# ═══════════════════════════════════════════════════════════
class CherryChatClient:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT):
        self._host = host
        self._port = port
        self._sock = None
        self._running = False
        self._connected = False

        self.user_id = 0
        self.user_name = ""
        self._friends = {}   # {id: name}
        self._groups = {}    # {id: name}

        self._print_lock = threading.Lock()
        self._login_event = threading.Event()
        self._login_result = None
        self._ack_event = threading.Event()
        self._ack_result = None

    # ── 连接 ──────────────────────────────────────────
    def connect(self) -> bool:
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(RECV_TIMEOUT)
            self._sock.connect((self._host, self._port))
            self._connected = True
            self._running = True
            return True
        except ConnectionRefusedError:
            self._eprint(f"无法连接 {self._host}:{self._port}，请确认服务端已启动")
            return False
        except Exception as e:
            self._eprint(f"连接失败: {e}")
            return False

    def disconnect(self):
        self._running = False
        self._connected = False
        if self._sock:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            self._sock.close()
            self._sock = None

    # ── 发送 ──────────────────────────────────────────
    def _send(self, msg: dict):
        if not self._sock:
            return
        try:
            raw = json.dumps(msg, ensure_ascii=False) + "\n"
            self._sock.sendall(raw.encode("utf-8"))
        except Exception as e:
            self._eprint(f"发送失败: {e}")
            self._running = False

    # ── 线程安全打印 ──────────────────────────────────
    def _sprint(self, *args, **kwargs):
        with self._print_lock:
            print(*args, **kwargs)

    def _eprint(self, text: str):
        self._sprint(f"{C.R}✘ {text}{C.X}")

    def _okprint(self, text: str):
        self._sprint(f"{C.G}✔ {text}{C.X}")

    def _infoprint(self, text: str):
        self._sprint(f"{C.C}ℹ {text}{C.X}")

    # ── 接收线程 ──────────────────────────────────────
    def _recv_loop(self):
        buf = b""
        while self._running:
            try:
                data = self._sock.recv(4096)
            except socket.timeout:
                continue
            except (ConnectionResetError, BrokenPipeError, OSError):
                if self._running:
                    self._sprint(f"\n{C.R}⚠ 与服务器断开连接{C.X}")
                self._running = False
                self._connected = False
                break

            if not data:
                if self._running:
                    self._sprint(f"\n{C.R}⚠ 服务器关闭了连接{C.X}")
                self._running = False
                self._connected = False
                break

            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line.strip():
                    try:
                        msg = json.loads(line.decode("utf-8"))
                        self._dispatch(msg)
                    except (json.JSONDecodeError, UnicodeDecodeError):
                        pass

    def _heartbeat_loop(self):
        while self._running and self._connected:
            time.sleep(HEARTBEAT_INTERVAL)
            if not self._running or not self._connected:
                break
            self._send({"msgid": PING_MSG})

    # ── 消息分发 ──────────────────────────────────────
    def _dispatch(self, msg: dict):
        msgid = msg.get("msgid", 0)
        if msgid == LOGIN_MSG_ACK:
            self._login_result = msg
            self._login_event.set()
        elif msgid == REG_MSG_ACK:
            self._ack_result = msg
            self._ack_event.set()
        elif msgid == ONE_CHAT_MSG:
            sid = msg.get("id", 0)
            if sid == self.user_id:
                return
            self._friends[sid] = msg.get("name", str(sid))
            self._sprint(f"\n{C.Y}{C.BOLD}💬 {msg.get('name','?')}({sid}):{C.X} {msg.get('msg','')}")
        elif msgid == GROUP_CHAT_MSG:
            sid = msg.get("id", 0)
            if sid == self.user_id:
                return
            gid = msg.get("groupid", 0)
            gn = self._groups.get(gid, f"群{gid}")
            self._sprint(f"\n{C.M}{C.BOLD}👥 [{gn}] {msg.get('name','?')}:{C.X} {msg.get('msg','')}")
        elif msgid == GROUP_CHAT_MSG_ACK:
            self._ack_result = msg
            self._ack_event.set()
        elif msgid == ADD_FRIEND_MSG_ACK:
            self._ack_result = msg
            self._ack_event.set()
        elif msgid == CREATE_GROUP_MSG_ACK:
            self._ack_result = msg
            self._ack_event.set()
        elif msgid == ADD_GROUP_MSG_ACK:
            self._ack_result = msg
            self._ack_event.set()
        # PONG_MSG 静默忽略

    def _wait_ack(self, timeout=5.0) -> dict | None:
        self._ack_event.clear()
        self._ack_result = None
        return self._ack_result if self._ack_event.wait(timeout) else None

    def _input(self, prompt: str) -> str:
        with self._print_lock:
            try:
                return input(prompt).strip()
            except (EOFError, KeyboardInterrupt):
                return ""

    # ═══════════════════════════════════════════════════
    # UI
    # ═══════════════════════════════════════════════════
    def _show_auth_menu(self):
        _clear()
        print(f"""{C.C}{C.BOLD}
╔══════════════════════════════════════════════╗
║      🗨️  CherryChat  终端聊天客户端          ║
║            {self._host}:{self._port:<5}                        ║
╚══════════════════════════════════════════════╝{C.X}

  {C.BOLD}[1]{C.X} 登录
  {C.BOLD}[2]{C.X} 注册新账号
  {C.BOLD}[0]{C.X} 退出
""")

    def _show_main_menu(self):
        _clear()
        fs = ", ".join(f"{n}({i})" for i, n in self._friends.items()) or "(暂无)"
        gs = ", ".join(f"{n}({i})" for i, n in self._groups.items()) or "(暂无)"

        # 计算标题填充
        tag = f"{self.user_name} (ID:{self.user_id})"
        pad = max(0, 24 - len(tag))
        print(f"""{C.C}{C.BOLD}
╔══════════════════════════════════════════════╗
║  🗨️  CherryChat — {tag}{" " * pad}║
╚══════════════════════════════════════════════╝{C.X}
  {C.G}📋 好友:{C.X} {fs}
  {C.M}👥 群组:{C.X} {gs}
{C.D}──────────────────────────────────────────────{C.X}
  {C.BOLD}[1]{C.X} 💬 一对一聊天
  {C.BOLD}[2]{C.X} 👤 添加好友
  {C.BOLD}[3]{C.X} 👥 创建群组
  {C.BOLD}[4]{C.X} ➕ 加入群组
  {C.BOLD}[5]{C.X} 📢 群聊
  {C.BOLD}[0]{C.X} 🚪 退出登录
{C.D}──────────────────────────────────────────────{C.X}
""")

    # ═══════════════════════════════════════════════════
    # 业务操作
    # ═══════════════════════════════════════════════════
    def do_register(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 注册新账号 ═══{C.X}\n")
        name = self._input("  用户名: ")
        if not name:
            return
        if len(name) < 2 or len(name) > 50:
            self._eprint("用户名需 2-50 个字符")
            self._input("按 Enter 返回...")
            return
        pwd = self._input("  密码:   ")
        if not pwd or len(pwd) < 3:
            self._eprint("密码至少 3 位")
            self._input("按 Enter 返回...")
            return

        self._send({"msgid": REG_MSG, "name": name, "password": _sha256(pwd)})
        r = self._wait_ack(5.0)

        if r is None:
            self._eprint("请求超时")
        elif r.get("errno", 1) == 0:
            self._okprint(f"注册成功！你的 ID 是 {r.get('id','?')}（登录不需要 ID，只需要用户名密码）")
        else:
            self._eprint(f"注册失败: {r.get('msg','未知错误')}")
        self._input("\n按 Enter 返回...")

    def do_login(self) -> bool:
        _clear()
        print(f"{C.C}{C.BOLD}═══ 登录 ═══{C.X}\n")
        name = self._input("  用户名: ")
        if not name:
            return False
        pwd = self._input("  密码:   ")
        if not pwd:
            return False

        self._login_event.clear()
        self._login_result = None
        self._send({"msgid": LOGIN_MSG, "name": name, "password": _sha256(pwd)})

        if not self._login_event.wait(5.0):
            self._eprint("登录超时")
            self._input("按 Enter 返回...")
            return False

        r = self._login_result
        if r is None:
            self._eprint("无响应")
            self._input("按 Enter 返回...")
            return False

        if r.get("errno", 1) == 0:
            self.user_id = r.get("id", 0)
            self.user_name = r.get("name", name)
            self._okprint(f"登录成功！欢迎 {self.user_name}")
            time.sleep(0.8)
            return True
        else:
            self._eprint(f"登录失败: {r.get('msg','用户名或密码错误')}")
            self._input("按 Enter 返回...")
            return False

    def do_one_chat(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 一对一聊天 ═══{C.X}\n")
        if not self._friends:
            self._infoprint("还没有好友，先添加好友吧（主菜单 [2]）")
            self._input("\n按 Enter 返回...")
            return

        print(f"  {C.G}好友列表:{C.X}")
        fl = list(self._friends.items())
        for i, (fid, fn) in enumerate(fl, 1):
            print(f"    [{i}] {fn} (ID:{fid})")
        print(f"    [0] 返回")

        try:
            c = int(self._input("\n  选择好友: "))
        except ValueError:
            return
        if c == 0 or c > len(fl):
            return
        fid, fn = fl[c - 1]

        _clear()
        print(f"{C.C}{C.BOLD}═══ 与 {fn} 聊天中 ═══{C.X}")
        print(f"  {C.D}(输入消息回车发送，/quit 退出){C.X}\n")

        while self._running and self._connected:
            try:
                t = self._input(f"  {C.BOLD}我:{C.X} ")
            except (EOFError, KeyboardInterrupt):
                break
            if not t:
                continue
            if t.lower() in ("/quit", "/exit", "/q"):
                break
            self._send({
                "msgid": ONE_CHAT_MSG,
                "id": self.user_id,
                "name": self.user_name,
                "toid": fid,
                "msg": t,
            })

    def do_add_friend(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 添加好友 ═══{C.X}\n")
        print(f"  {C.D}请输入对方的数字 ID（让对方在主菜单查看）{C.X}")
        try:
            fid = int(self._input("\n  好友 ID: "))
        except ValueError:
            return
        if fid == self.user_id:
            self._eprint("不能添加自己")
            self._input("按 Enter 返回...")
            return
        if fid in self._friends:
            self._infoprint("已经是好友了")
            self._input("按 Enter 返回...")
            return

        self._send({"msgid": ADD_FRIEND_MSG, "id": self.user_id, "friendid": fid})
        r = self._wait_ack(5.0)
        if r is None:
            self._eprint("请求超时")
        elif r.get("errno", 1) == 0:
            self._okprint("添加好友成功！")
            self._friends[fid] = str(fid)
        else:
            self._eprint(f"添加失败: {r.get('msg','未知错误')}")
        self._input("\n按 Enter 返回...")

    def do_create_group(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 创建群组 ═══{C.X}\n")
        gn = self._input("  群组名称: ")
        if not gn:
            return
        self._send({"msgid": CREATE_GROUP_MSG, "id": self.user_id, "groupname": gn})
        r = self._wait_ack(5.0)
        if r is None:
            self._eprint("请求超时")
        elif r.get("errno", 1) == 0:
            gid = r.get("groupid", 0)
            self._groups[gid] = gn
            self._okprint(f"创建成功！'{gn}' (ID:{gid})")
        else:
            self._eprint(f"创建失败: {r.get('msg','未知错误')}")
        self._input("\n按 Enter 返回...")

    def do_join_group(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 加入群组 ═══{C.X}\n")
        print(f"  {C.D}请输入群组 ID（由群主告知）{C.X}")
        try:
            gid = int(self._input("\n  群组 ID: "))
        except ValueError:
            return
        if gid in self._groups:
            self._infoprint("你已经在这个群里了")
            self._input("按 Enter 返回...")
            return
        self._send({"msgid": ADD_GROUP_MSG, "id": self.user_id, "groupid": gid})
        r = self._wait_ack(5.0)
        if r is None:
            self._eprint("请求超时")
        elif r.get("errno", 1) == 0:
            self._okprint("加入成功！")
            self._groups[gid] = f"群{gid}"
        else:
            self._eprint(f"加入失败: {r.get('msg','未知错误')}")
        self._input("\n按 Enter 返回...")

    def do_group_chat(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 群聊 ═══{C.X}\n")
        if not self._groups:
            self._infoprint("还没有群组，先创建或加入群组吧")
            self._input("\n按 Enter 返回...")
            return

        print(f"  {C.M}我的群组:{C.X}")
        gl = list(self._groups.items())
        for i, (gid, gn) in enumerate(gl, 1):
            print(f"    [{i}] {gn} (ID:{gid})")
        print(f"    [0] 返回")
        try:
            c = int(self._input("\n  选择群组: "))
        except ValueError:
            return
        if c == 0 or c > len(gl):
            return
        gid, gn = gl[c - 1]

        _clear()
        print(f"{C.C}{C.BOLD}═══ 群聊: {gn} ═══{C.X}")
        print(f"  {C.D}(输入消息回车发送，/quit 退出){C.X}\n")
        while self._running and self._connected:
            try:
                t = self._input(f"  {C.BOLD}我:{C.X} ")
            except (EOFError, KeyboardInterrupt):
                break
            if not t:
                continue
            if t.lower() in ("/quit", "/exit", "/q"):
                break
            self._send({
                "msgid": GROUP_CHAT_MSG,
                "id": self.user_id,
                "name": self.user_name,
                "groupid": gid,
                "msg": t,
            })

    # ═══════════════════════════════════════════════════
    # 主循环
    # ═══════════════════════════════════════════════════
    def run(self):
        signal.signal(signal.SIGINT, lambda s, f: self._safe_exit())

        if not self.connect():
            return

        t = threading.Thread(target=self._recv_loop, daemon=True)
        t.start()

        # 认证循环
        while self._running:
            self._show_auth_menu()
            c = self._input("  请选择: ")
            if c == "1":
                if self.do_login():
                    break
            elif c == "2":
                self.do_register()
            elif c == "0":
                self._sprint(f"\n{C.D}再见！{C.X}")
                self.disconnect()
                return

        if not self._running or not self._connected:
            return

        hb = threading.Thread(target=self._heartbeat_loop, daemon=True)
        hb.start()

        # 主菜单循环
        while self._running and self._connected:
            self._show_main_menu()
            c = self._input("  请选择: ")
            if c == "1":
                self.do_one_chat()
            elif c == "2":
                self.do_add_friend()
            elif c == "3":
                self.do_create_group()
            elif c == "4":
                self.do_join_group()
            elif c == "5":
                self.do_group_chat()
            elif c == "0":
                self._sprint(f"\n{C.D}已退出登录{C.X}")
                self.user_id = 0
                self.user_name = ""
                self._friends.clear()
                self._groups.clear()
                break

        self.disconnect()

    def _safe_exit(self):
        self._sprint(f"\n{C.D}正在退出...{C.X}")
        self._running = False
        self.disconnect()
        sys.exit(0)


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    while True:
        c = CherryChatClient(host, port)
        c.run()
        # 退出登录后重新创建实例（允许再次登录）
        if not c._running:
            break
