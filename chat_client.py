#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════╗
║        CherryChat v2 — QQ风格终端聊天客户端              ║
║                                                          ║
║  功能: 注册 | 登录 | 好友 | 群组 | 一对一聊天 | 群聊     ║
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
HEARTBEAT_INTERVAL = 30
RECV_TIMEOUT = 1.0

# ═══════════════════════════════════════════════════════════
# 消息类型（与 include/public.h 同步）
# ═══════════════════════════════════════════════════════════
(
    LOGIN_MSG, LOGIN_MSG_ACK, REG_MSG, REG_MSG_ACK,
    ONE_CHAT_MSG, ADD_FRIEND_MSG, CREATE_GROUP_MSG, ADD_GROUP_MSG,
    GROUP_CHAT_MSG, ADD_FRIEND_MSG_ACK, CREATE_GROUP_MSG_ACK,
    ADD_GROUP_MSG_ACK, GROUP_CHAT_MSG_ACK,
    PING_MSG, PONG_MSG,
    # 新增消息类型
    DEL_FRIEND_MSG, DEL_FRIEND_MSG_ACK,   # 16, 17
    QUIT_GROUP_MSG, QUIT_GROUP_MSG_ACK,    # 18, 19
    FRIEND_STATUS_REQ, FRIEND_STATUS_ACK,  # 20, 21
    FRIEND_LIST_MSG, GROUP_LIST_MSG,       # 22, 23
) = range(1, 24)

# ═══════════════════════════════════════════════════════════
# ANSI 颜色
# ═══════════════════════════════════════════════════════════
class C:
    R = "\033[91m"; G = "\033[92m"; Y = "\033[93m"
    B = "\033[94m"; M = "\033[95m"; C = "\033[96m"
    W = "\033[97m"; D = "\033[2m"; X = "\033[0m"
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
        self._friends = {}    # {id: {"name": str, "online": bool}}
        self._groups = {}     # {id: {"name": str, "members": int}}

        self._print_lock = threading.Lock()
        self._login_event = threading.Event()
        self._login_result = None
        self._ack_event = threading.Event()
        self._ack_result = None

        # 聊天模式标记
        self._chat_target = None   # (type, id, name)  如 ("friend", 1, "mary")

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

    def _send(self, msg: dict):
        if not self._sock:
            return
        try:
            raw = json.dumps(msg, ensure_ascii=False) + "\n"
            self._sock.sendall(raw.encode("utf-8"))
        except Exception:
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
            name = msg.get("name", str(sid))
            if sid not in self._friends:
                self._friends[sid] = {"name": name, "online": True}
            else:
                self._friends[sid]["name"] = name
                self._friends[sid]["online"] = True
            text = msg.get("msg", "")
            if text:
                if self._chat_target and self._chat_target[1] == sid:
                    self._sprint(f"\n  {C.Y}{C.BOLD}💬 {name}:{C.X} {text}")
                    self._sprint(f"  {C.BOLD}我:{C.X} ", end="", flush=True)
                else:
                    self._sprint(f"\n{C.Y}{C.BOLD}💬 [{name}] 发来消息:{C.X} {text}")

        elif msgid == GROUP_CHAT_MSG:
            sid = msg.get("id", 0)
            if sid == self.user_id:
                return
            gid = msg.get("groupid", 0)
            gn = self._groups.get(gid, {}).get("name", f"群{gid}")
            text = msg.get("msg", "")
            if self._chat_target and self._chat_target[0] == "group" and self._chat_target[1] == gid:
                self._sprint(f"\n  {C.M}{C.BOLD}👥 {msg.get('name','?')}:{C.X} {text}")
                self._sprint(f"  {C.BOLD}我:{C.X} ", end="", flush=True)
            else:
                self._sprint(f"\n{C.M}{C.BOLD}👥 [{gn}] {msg.get('name','?')}:{C.X} {text}")

        elif msgid in (ADD_FRIEND_MSG_ACK, CREATE_GROUP_MSG_ACK,
                       ADD_GROUP_MSG_ACK, GROUP_CHAT_MSG_ACK,
                       DEL_FRIEND_MSG_ACK, QUIT_GROUP_MSG_ACK):
            self._ack_result = msg
            self._ack_event.set()

        elif msgid == FRIEND_LIST_MSG:
            friends = msg.get("friends", [])
            for f in friends:
                self._friends[f["id"]] = {"name": f["name"], "online": f.get("online", False)}

        elif msgid == FRIEND_STATUS_ACK:
            friends = msg.get("friends", [])
            for f in friends:
                fid = f["id"]
                if fid in self._friends:
                    self._friends[fid]["online"] = f.get("online", False)
                else:
                    self._friends[fid] = {"name": f["name"], "online": f.get("online", False)}

        elif msgid == GROUP_LIST_MSG:
            groups = msg.get("groups", [])
            for g in groups:
                self._groups[g["id"]] = {"name": g["name"], "members": g.get("members", 0)}

    def _wait_ack(self, timeout=5.0) -> dict | None:
        self._ack_event.clear()
        self._ack_result = None
        return self._ack_result if self._ack_event.wait(timeout) else None

    def _input(self, prompt: str) -> str:
        try:
            return input(prompt).strip()
        except (EOFError, KeyboardInterrupt):
            return ""

    # ═══════════════════════════════════════════════════
    # UI 组件
    # ═══════════════════════════════════════════════════
    def _draw_header(self, title: str, subtitle: str = ""):
        print(f"{C.C}{C.BOLD}╔══════════════════════════════════════════════╗{C.X}")
        print(f"{C.C}{C.BOLD}║{C.X}  {C.BOLD}🗨️  {title:<37}{C.C}{C.BOLD}║{C.X}")
        if subtitle:
            print(f"{C.C}{C.BOLD}║{C.X}  {C.D}{subtitle:<39}{C.C}{C.BOLD}║{C.X}")
        print(f"{C.C}{C.BOLD}╚══════════════════════════════════════════════╝{C.X}")

    def _draw_divider(self):
        print(f"{C.D}──────────────────────────────────────────────{C.X}")

    # ═══════════════════════════════════════════════════
    # 认证页面
    # ═══════════════════════════════════════════════════
    def _show_auth_menu(self):
        _clear()
        self._draw_header("CherryChat", f"{self._host}:{self._port}")
        print()
        print(f"  {C.BOLD}[1]{C.X}  🔑 登录")
        print(f"  {C.BOLD}[2]{C.X}  📝 注册新账号")
        print(f"  {C.BOLD}[0]{C.X}  👋 退出")
        print()

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
            self._okprint(f"注册成功！ID: {r.get('id','?')}")
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
            time.sleep(0.6)
            return True
        else:
            self._eprint(f"登录失败: {r.get('msg','用户名或密码错误')}")
            self._input("按 Enter 返回...")
            return False

    # ═══════════════════════════════════════════════════
    # 主菜单
    # ═══════════════════════════════════════════════════
    def _show_main_menu(self):
        _clear()
        fc = len(self._friends)
        gc = len(self._groups)
        tag = f"{self.user_name} (ID:{self.user_id})"
        self._draw_header("CherryChat", tag)
        print()
        print(f"  {C.BOLD}[1]{C.X}  👤 我的好友  {C.D}({fc}人){C.X}")
        print(f"  {C.BOLD}[2]{C.X}  👥 我的群组  {C.D}({gc}个){C.X}")
        self._draw_divider()
        print(f"  {C.BOLD}[3]{C.X}  ➕ 添加好友")
        print(f"  {C.BOLD}[4]{C.X}  🆕 创建群组")
        print(f"  {C.BOLD}[5]{C.X}  🔍 加入群组")
        self._draw_divider()
        print(f"  {C.BOLD}[0]{C.X}  🚪 退出登录")
        print()

    # ═══════════════════════════════════════════════════
    # 好友列表页
    # ═══════════════════════════════════════════════════
    def do_friend_list(self):
        while self._running and self._connected:
            _clear()
            # ⭐ 进入好友列表时请求刷新状态
            self._send({"msgid": FRIEND_STATUS_REQ, "id": self.user_id})
            time.sleep(0.3)
            self._draw_header("👤 我的好友", f"共 {len(self._friends)} 位好友")
            print()
            if not self._friends:
                print(f"  {C.D}(暂无好友，去添加吧){C.X}")
                print()
                print(f"  {C.BOLD}[0]{C.X} 返回")
                print()
                c = self._input("  请选择: ")
                return

            fl = list(self._friends.items())
            for i, (fid, info) in enumerate(fl, 1):
                name = info["name"]
                online = info.get("online", False)
                status = f"{C.G}🟢 在线{C.X}" if online else f"{C.D}⚫ 离线{C.X}"
                print(f"  {C.BOLD}[{i}]{C.X} {name} (ID:{fid})  {status}")
            print()
            self._draw_divider()
            print(f"  {C.BOLD}[编号]{C.X} 开始聊天  |  {C.BOLD}[d+编号]{C.X} 删除好友  |  {C.BOLD}[0]{C.X} 返回")
            print()

            raw = self._input("  请选择: ").lower()
            if raw == "0" or raw == "":
                return

            # 删除好友: d1, d2 ...
            if raw.startswith("d"):
                try:
                    idx = int(raw[1:]) - 1
                    if 0 <= idx < len(fl):
                        fid, info = fl[idx]
                        self.do_delete_friend(fid, info["name"])
                except ValueError:
                    pass
                continue

            # 开始聊天
            try:
                idx = int(raw) - 1
                if 0 <= idx < len(fl):
                    fid, info = fl[idx]
                    self.do_one_chat(fid, info["name"])
            except ValueError:
                pass

    # ═══════════════════════════════════════════════════
    # 群组列表页
    # ═══════════════════════════════════════════════════
    def do_group_list(self):
        while self._running and self._connected:
            _clear()
            self._draw_header("👥 我的群组", f"共 {len(self._groups)} 个群组")
            print()
            if not self._groups:
                print(f"  {C.D}(暂无群组，去创建或加入吧){C.X}")
                print()
                print(f"  {C.BOLD}[0]{C.X} 返回")
                print()
                c = self._input("  请选择: ")
                return

            gl = list(self._groups.items())
            for i, (gid, info) in enumerate(gl, 1):
                name = info["name"]
                mc = info.get("members", "?")
                print(f"  {C.BOLD}[{i}]{C.X} {C.M}{name}{C.X}  {C.D}({mc}人, ID:{gid}){C.X}")
            print()
            self._draw_divider()
            print(f"  {C.BOLD}[编号]{C.X} 进入群聊  |  {C.BOLD}[q+编号]{C.X} 退出群组  |  {C.BOLD}[0]{C.X} 返回")
            print()

            raw = self._input("  请选择: ").lower()
            if raw == "0" or raw == "":
                return

            if raw.startswith("q"):
                try:
                    idx = int(raw[1:]) - 1
                    if 0 <= idx < len(gl):
                        gid, info = gl[idx]
                        self.do_quit_group(gid, info["name"])
                except ValueError:
                    pass
                continue

            try:
                idx = int(raw) - 1
                if 0 <= idx < len(gl):
                    gid, info = gl[idx]
                    self.do_group_chat(gid, info["name"])
            except ValueError:
                pass

    # ═══════════════════════════════════════════════════
    # 一对一聊天
    # ═══════════════════════════════════════════════════
    def do_one_chat(self, fid: int, fname: str):
        self._chat_target = ("friend", fid, fname)
        _clear()
        self._draw_header(f"💬 与 {fname} 聊天中", "/quit 退出")
        print()

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

        self._chat_target = None

    # ═══════════════════════════════════════════════════
    # 群聊
    # ═══════════════════════════════════════════════════
    def do_group_chat(self, gid: int, gname: str):
        self._chat_target = ("group", gid, gname)
        _clear()
        self._draw_header(f"👥 群聊: {gname}", "/quit 退出")
        print()

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

        self._chat_target = None

    # ═══════════════════════════════════════════════════
    # 操作：添加好友 / 删除好友 / 创建群组 / 加入群组 / 退出群组
    # ═══════════════════════════════════════════════════
    def do_add_friend(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 添加好友 ═══{C.X}\n")
        print(f"  {C.D}请输入对方的数字 ID{C.X}")
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
            self._friends[fid] = {"name": str(fid), "online": False}
        else:
            self._eprint(f"添加失败: {r.get('msg','未知错误')}")
        self._input("\n按 Enter 返回...")

    def do_delete_friend(self, fid: int, fname: str):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 删除好友 ═══{C.X}\n")
        print(f"  确定删除好友 {C.Y}{fname}{C.X} 吗？")
        print(f"  {C.BOLD}[y]{C.X} 确认  |  {C.BOLD}[n]{C.X} 取消")
        c = self._input("\n  请选择: ").lower()
        if c != "y":
            return
        self._send({"msgid": DEL_FRIEND_MSG, "id": self.user_id, "friendid": fid})
        r = self._wait_ack(5.0)
        if r and r.get("errno", 1) == 0:
            self._friends.pop(fid, None)
            self._okprint("已删除好友")
        else:
            self._eprint(f"删除失败: {r.get('msg','') if r else '超时'}")
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
            self._groups[gid] = {"name": gn, "members": 1}
            self._okprint(f"创建成功！'{gn}' (ID:{gid})")
        else:
            self._eprint(f"创建失败: {r.get('msg','未知错误')}")
        self._input("\n按 Enter 返回...")

    def do_join_group(self):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 加入群组 ═══{C.X}\n")
        print(f"  {C.D}请输入群组 ID{C.X}")
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
            self._groups[gid] = {"name": f"群{gid}", "members": 1}
        else:
            self._eprint(f"加入失败: {r.get('msg','未知错误')}")
        self._input("\n按 Enter 返回...")

    def do_quit_group(self, gid: int, gname: str):
        _clear()
        print(f"{C.C}{C.BOLD}═══ 退出群组 ═══{C.X}\n")
        print(f"  确定退出 {C.M}{gname}{C.X} 吗？")
        print(f"  {C.BOLD}[y]{C.X} 确认  |  {C.BOLD}[n]{C.X} 取消")
        c = self._input("\n  请选择: ").lower()
        if c != "y":
            return
        self._send({"msgid": QUIT_GROUP_MSG, "id": self.user_id, "groupid": gid})
        r = self._wait_ack(5.0)
        if r and r.get("errno", 1) == 0:
            self._groups.pop(gid, None)
            self._okprint("已退出群组")
        else:
            self._eprint(f"退出失败: {r.get('msg','') if r else '超时'}")
        self._input("\n按 Enter 返回...")

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
                    # 等待离线消息到达并显示
                    time.sleep(0.8)
                    self._sprint(f"\n{C.D}══════════════════════════════════════{C.X}")
                    self._input("按 Enter 进入主菜单...")
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
                self.do_friend_list()
            elif c == "2":
                self.do_group_list()
            elif c == "3":
                self.do_add_friend()
            elif c == "4":
                self.do_create_group()
            elif c == "5":
                self.do_join_group()
            elif c == "0":
                self._sprint(f"\n{C.D}已退出登录{C.X}")
                self.user_id = 0
                self.user_name = ""
                self._friends.clear()
                self._groups.clear()
                self._chat_target = None
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
        if not c._running:
            break