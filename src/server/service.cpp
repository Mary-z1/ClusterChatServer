#include "service.h"
#include "public.h"
#include "db.h"
#include "model.h"
#include "logger.h"
#include "config.h"
#include <iostream>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

// ==================== SHA256 哈希工具 ====================
static std::string sha256(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()),
           input.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    return oss.str();
}

// ==================== 输入校验 ====================
static std::string validateName(const std::string& name) {
    if (name.empty())          return "name cannot be empty";
    if (name.size() > 50)      return "name too long (max 50)";
    for (char c : name)
        if (!isalnum(c) && c != '_' && c != '-')
            return "name contains invalid characters";
    return "";
}

static std::string validatePassword(const std::string& pw) {
    if (pw.empty())            return "password cannot be empty";
    if (pw.size() < 6)         return "password too short (min 6)";
    if (pw.size() > 128)       return "password too long (max 128)";
    return "";
}

// ==================== 快速错误响应 ====================
static void sendError(const TcpConnectionPtr& conn,
                      int msgid, int errno_, const std::string& msg) {
    json resp;
    resp["msgid"] = msgid;
    resp["errno"] = errno_;
    resp["errmsg"] = msg;
    conn->send(resp.dump() + "\n");
}

// ==================== 单例 ====================
ChatService* ChatService::instance() {
    static ChatService service;
    return &service;
}

ChatService::ChatService() {
    // ====== 登录 ======
    handlers_[LOGIN_MSG] = [](const TcpConnectionPtr& conn,
                               json& js, Timestamp) {
        std::string name = js.value("name", "");
        std::string password = js.value("password", "");
        int uid = 0;   // ⭐ 提到外层，供离线推送使用

        // ⭐ 输入校验
        std::string err = validateName(name);
        if (!err.empty()) { sendError(conn, LOGIN_MSG_ACK, 10, err); return; }
        err = validatePassword(password);
        if (!err.empty()) { sendError(conn, LOGIN_MSG_ACK, 11, err); return; }

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) { sendError(conn, LOGIN_MSG_ACK, 99, "database unavailable"); return; }

        json response;
        response["msgid"] = LOGIN_MSG_ACK;

        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT id, password FROM User WHERE name='%s'",
                 mysql->escape(name).c_str());

        MYSQL_RES* res = mysql->query(sql);
        if (!res) {
            response["errno"] = 1; response["errmsg"] = "database error";
        } else {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (!row) {
                response["errno"] = 1; response["errmsg"] = "user not exists";
            } else {
                if (sha256(password) != std::string(row[1])) {
                    response["errno"] = 2; response["errmsg"] = "wrong password";
                } else {
                    uid = std::stoi(row[0]);    // ⭐ 赋值给外层 uid
                    response["errno"] = 0;
                    response["id"] = uid;
                    response["name"] = name;

                    // 更新在线状态
                    snprintf(sql, sizeof(sql),
                             "UPDATE User SET state='online' WHERE id=%d", uid);
                    mysql->update(sql);
                    {
                        std::lock_guard<std::mutex> lk(
                            ChatService::instance()->onlineMtx_);
                        ChatService::instance()->onlineUsers_[uid] = conn;
                    }
                    ChatService::instance()->resetActiveTime(uid);  // ← 新增
                    LOG_INFO("[LOGIN] %s (id=%d)", name.c_str(), uid);
                }
            }
            mysql_free_result(res);
        }
        conn->send(response.dump() + "\n");

// ⭐ 登录成功 → 先推送离线消息，再推送好友列表
if (uid > 0) {
    // ① 推送离线消息
    {
        char offSql[2048];
        snprintf(offSql, sizeof(offSql),
            "SELECT message FROM OfflineMessage WHERE userid = %d", uid);
        MYSQL_RES* offRes = mysql->query(offSql);
        if (offRes) {
            MYSQL_ROW offRow;
            while ((offRow = mysql_fetch_row(offRes))) {
                conn->send(std::string(offRow[0]) + "\n");
            }
            mysql_free_result(offRes);
            // 推送完立即删除
            snprintf(offSql, sizeof(offSql),
                "DELETE FROM OfflineMessage WHERE userid = %d", uid);
            mysql->update(offSql);
            LOG_INFO("[LOGIN] offline msgs pushed & cleared, uid=%d", uid);
        }
    }
    // ② 推送好友列表（保留现有代码不变）
    char friendSql[512];
    snprintf(friendSql, sizeof(friendSql),
        "SELECT u.id, u.name FROM Friend f "
        "JOIN User u ON f.friendid = u.id "
        "WHERE f.userid = %d", uid);
    MYSQL_RES* fRes = mysql->query(friendSql);
    if (fRes) {
        MYSQL_ROW fRow;
        while ((fRow = mysql_fetch_row(fRes))) {
            json fj;
            fj["msgid"] = ONE_CHAT_MSG;
            fj["id"] = std::stoi(fRow[0]);
            fj["name"] = fRow[1];
            fj["msg"] = "";    // 空消息 → 客户端不打印，仅填充好友字典
            conn->send(fj.dump() + "\n");
        }
        mysql_free_result(fRes);
        LOG_INFO("[LOGIN] friend list pushed, uid=%d", uid);
    }
}
    };

    // ====== 注册 ======
    handlers_[REG_MSG] = [](const TcpConnectionPtr& conn,
                             json& js, Timestamp) {
        std::string name = js.value("name", "");
        std::string password = js.value("password", "");
        LOG_INFO("[REG_MSG] received: name=%s", name.c_str());

        // ⭐ 输入校验
        std::string err = validateName(name);
        if (!err.empty()) { sendError(conn, REG_MSG_ACK, 10, err); return; }
        err = validatePassword(password);
        if (!err.empty()) { sendError(conn, REG_MSG_ACK, 11, err); return; }

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) { sendError(conn, REG_MSG_ACK, 99, "database unavailable"); return; }

        json response;
        response["msgid"] = REG_MSG_ACK;

        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT id FROM User WHERE name='%s'",
                 mysql->escape(name).c_str());

        MYSQL_RES* res = mysql->query(sql);
        if (res && mysql_fetch_row(res)) {
            response["errno"] = 1; response["errmsg"] = "user already exists";
            mysql_free_result(res);
        } else {
            if (res) mysql_free_result(res);

            std::string hashed = sha256(password);
            snprintf(sql, sizeof(sql),
                     "INSERT INTO User (name, password) VALUES ('%s', '%s')",
                     mysql->escape(name).c_str(),
                     mysql->escape(hashed).c_str());

            if (mysql->update(sql)) {
                int newId = mysql_insert_id(mysql->getConnection());
                response["errno"] = 0;
                response["id"] = newId;
                response["name"] = name;
                LOG_INFO("[REG] new user: %s (id=%d)", name.c_str(), newId);
            } else {
                response["errno"] = 2; response["errmsg"] = "insert failed";
            }
        }
        conn->send(response.dump() + "\n");
    };

        // ─────────── 一对一聊天 ───────────
    handlers_[ONE_CHAT_MSG] = [](const TcpConnectionPtr& conn, json& js, Timestamp) {
        int to_id   = js.value("toid", 0);
        std::string msg = js.value("msg", "");

        if (msg.empty() || msg.size() > 5000) return;

        auto* svc = ChatService::instance();

        // ① 先查本地在线表（快速路径）
        {
            std::lock_guard<std::mutex> lock(svc->onlineMtx_);
            auto it = svc->onlineUsers_.find(to_id);
            if (it != svc->onlineUsers_.end()) {
                // 目标在本机在线 → 直接转发
                it->second->send(js.dump() + "\n");
                                LOG_INFO("[CHAT] local forward to uid=%d", to_id);
                return;
            }
        }

        // ② 不在本机 → 发布到 Redis（其他服务器可能会收到并转发）
        svc->redis_.publish("chat_server", js.dump());

        // ③ 持久化离线消息（源服务器负责兜底）
        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) return;

        std::string escaped = mysql->escape(js.dump());
        char sql[8192];
        snprintf(sql, sizeof(sql),
            "INSERT INTO OfflineMessage (userid, message) VALUES (%d, '%s')",
            to_id, escaped.c_str());
        mysql->update(sql);

                LOG_INFO("[OFFLINE] msg stored for uid=%d", to_id);
    };
        // ========== 阶段7新增：添加好友 ==========
    handlers_[ADD_FRIEND_MSG] = [](const TcpConnectionPtr& conn,
                                    json& js, Timestamp) {
        int uid = ChatService::instance()->getUidByConn(conn);
        if (uid == -1) {
            sendError(conn, ADD_FRIEND_MSG_ACK, 1, "not logged in");
            return;
        }

        int friendid = js.value("friendid", 0);
        if (friendid <= 0) {
            sendError(conn, ADD_FRIEND_MSG_ACK, 2, "invalid friendid");
            return;
        }
        if (friendid == uid) {
            sendError(conn, ADD_FRIEND_MSG_ACK, 3, "cannot add yourself");
            return;
        }

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) {
            sendError(conn, ADD_FRIEND_MSG_ACK, 99, "database unavailable");
            return;
        }

        // 检查对方是否存在
        char checkSql[128];
        snprintf(checkSql, sizeof(checkSql),
                 "SELECT id FROM User WHERE id=%d", friendid);
        MYSQL_RES* res = mysql->query(checkSql);
        if (!res || !mysql_fetch_row(res)) {
            if (res) mysql_free_result(res);
            sendError(conn, ADD_FRIEND_MSG_ACK, 4, "user not exists");
            return;
        }
        mysql_free_result(res);

        // 检查是否已是好友
        snprintf(checkSql, sizeof(checkSql),
                 "SELECT userid FROM Friend WHERE userid=%d AND friendid=%d",
                 uid, friendid);
        res = mysql->query(checkSql);
        if (res && mysql_fetch_row(res)) {
            mysql_free_result(res);
            sendError(conn, ADD_FRIEND_MSG_ACK, 5, "already friends");
            return;
        }
        if (res) mysql_free_result(res);

        // 插入好友关系
        if (FriendModel::add(mysql.get(), uid, friendid)) {
            json resp;
            resp["msgid"] = ADD_FRIEND_MSG_ACK;
            resp["errno"] = 0;
            conn->send(resp.dump() + "\n");
            LOG_INFO("[FRIEND] uid=%d added friend=%d", uid, friendid);
            return;
        }
        sendError(conn, ADD_FRIEND_MSG_ACK, 6, "add friend failed");
    };

    // ========== 阶段7新增：创建群组 ==========
    handlers_[CREATE_GROUP_MSG] = [](const TcpConnectionPtr& conn,
                                      json& js, Timestamp) {
        int uid = ChatService::instance()->getUidByConn(conn);
        if (uid == -1) {
            sendError(conn, CREATE_GROUP_MSG_ACK, 1, "not logged in");
            return;
        }

        std::string groupname = js.value("groupname", "");
        if (groupname.empty() || groupname.size() > 100) {
            sendError(conn, CREATE_GROUP_MSG_ACK, 2, "invalid group name");
            return;
        }

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) {
            sendError(conn, CREATE_GROUP_MSG_ACK, 99, "database unavailable");
            return;
        }

        int groupid = GroupModel::create(mysql.get(), groupname, uid);
        if (groupid == -1) {
            sendError(conn, CREATE_GROUP_MSG_ACK, 3, "create group failed");
            return;
        }

        // 创建者自动加入群组，角色为 creator
        GroupModel::addMember(mysql.get(), groupid, uid, "creator");

        json resp;
        resp["msgid"] = CREATE_GROUP_MSG_ACK;
        resp["errno"] = 0;
        resp["groupid"] = groupid;
        resp["groupname"] = groupname;
        conn->send(resp.dump() + "\n");
        LOG_INFO("[GROUP] uid=%d created group '%s' (id=%d)", uid, groupname.c_str(), groupid);
    };
    // ========== 阶段7新增：加入群组 ==========
    handlers_[ADD_GROUP_MSG] = [](const TcpConnectionPtr& conn,
                                   json& js, Timestamp) {
        int uid = ChatService::instance()->getUidByConn(conn);
        if (uid == -1) {
            sendError(conn, ADD_GROUP_MSG_ACK, 1, "not logged in");
            return;
        }

        int groupid = js.value("groupid", 0);
        if (groupid <= 0) {
            sendError(conn, ADD_GROUP_MSG_ACK, 2, "invalid groupid");
            return;
        }

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) {
            sendError(conn, ADD_GROUP_MSG_ACK, 99, "database unavailable");
            return;
        }

        // 检查群是否存在
        char checkSql[128];
        snprintf(checkSql, sizeof(checkSql),
                 "SELECT id FROM GroupInfo WHERE id=%d", groupid);
        MYSQL_RES* res = mysql->query(checkSql);
        if (!res || !mysql_fetch_row(res)) {
            if (res) mysql_free_result(res);
            sendError(conn, ADD_GROUP_MSG_ACK, 3, "group not exists");
            return;
        }
        mysql_free_result(res);

        // 检查是否已在群中
        snprintf(checkSql, sizeof(checkSql),
                 "SELECT userid FROM GroupUser WHERE groupid=%d AND userid=%d",
                 groupid, uid);
        res = mysql->query(checkSql);
        if (res && mysql_fetch_row(res)) {
            mysql_free_result(res);
            sendError(conn, ADD_GROUP_MSG_ACK, 4, "already in group");
            return;
        }
        if (res) mysql_free_result(res);

        if (GroupModel::addMember(mysql.get(), groupid, uid, "member")) {
            json resp;
            resp["msgid"] = ADD_GROUP_MSG_ACK;
            resp["errno"] = 0;
            resp["groupid"] = groupid;
            conn->send(resp.dump() + "\n");
            LOG_INFO("[GROUP] uid=%d joined group %d", uid, groupid);
        } else {
            sendError(conn, ADD_GROUP_MSG_ACK, 5, "join group failed");
        }
    };

    // ========== 阶段7新增：群组聊天 ==========
    handlers_[GROUP_CHAT_MSG] = [](const TcpConnectionPtr& conn,
                                    json& js, Timestamp) {
        ChatService::instance()->handleGroupChat(conn, js);
    };

    // ==================== Redis 跨服通信初始化 ====================
    auto* cfg = Config::instance();
    if (redis_.connect(cfg->getString("redis", "host", "127.0.0.1"),
                       cfg->getInt("redis", "port", 6379))) {
        redis_.setNotifyCallback([this](const std::string&, const std::string& msg) {
            onRedisMessage("chat_server", msg);
        });
        redis_.subscribe("chat_server");
        LOG_INFO("[Redis] subscribed to channel 'chat_server'");
    } else {
        LOG_ERROR("[Redis] WARNING: Redis unavailable, cross-server chat disabled");
    }
        // ========== 删除好友 ==========
    handlers_[DEL_FRIEND_MSG] = [](const TcpConnectionPtr& conn,
                                    json& js, Timestamp) {
        int uid = ChatService::instance()->getUidByConn(conn);
        if (uid == -1) {
            sendError(conn, DEL_FRIEND_MSG_ACK, 1, "not logged in");
            return;
        }

        int friendid = js.value("friendid", 0);
        if (friendid <= 0) {
            sendError(conn, DEL_FRIEND_MSG_ACK, 2, "invalid friendid");
            return;
        }

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) {
            sendError(conn, DEL_FRIEND_MSG_ACK, 99, "database unavailable");
            return;
        }

        // 双向删除：A删B 且 B删A
        char sql[256];
        snprintf(sql, sizeof(sql),
            "DELETE FROM Friend WHERE (userid=%d AND friendid=%d) OR (userid=%d AND friendid=%d)",
            uid, friendid, friendid, uid);
        mysql->update(sql);

        json resp;
        resp["msgid"] = DEL_FRIEND_MSG_ACK;
        resp["errno"] = 0;
        conn->send(resp.dump() + "\n");
        LOG_INFO("[FRIEND] uid=%d deleted friend=%d", uid, friendid);
    };

    // ========== 退出群组 ==========
    handlers_[QUIT_GROUP_MSG] = [](const TcpConnectionPtr& conn,
                                    json& js, Timestamp) {
        int uid = ChatService::instance()->getUidByConn(conn);
        if (uid == -1) {
            sendError(conn, QUIT_GROUP_MSG_ACK, 1, "not logged in");
            return;
        }

        int groupid = js.value("groupid", 0);
        if (groupid <= 0) {
            sendError(conn, QUIT_GROUP_MSG_ACK, 2, "invalid groupid");
            return;
        }

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) {
            sendError(conn, QUIT_GROUP_MSG_ACK, 99, "database unavailable");
            return;
        }

        char sql[256];
        snprintf(sql, sizeof(sql),
            "DELETE FROM GroupUser WHERE groupid=%d AND userid=%d",
            groupid, uid);
        mysql->update(sql);

        json resp;
        resp["msgid"] = QUIT_GROUP_MSG_ACK;
        resp["errno"] = 0;
        conn->send(resp.dump() + "\n");
        LOG_INFO("[GROUP] uid=%d quit group %d", uid, groupid);
    };
    // ========== 阶段8d：心跳 PING / PONG ==========
    handlers_[PING_MSG] = [](const TcpConnectionPtr& conn,
                              json&, Timestamp) {
        json resp;
        resp["msgid"] = PONG_MSG;
        conn->send(resp.dump() + "\n");
    };
    handlers_[PONG_MSG] = [](const TcpConnectionPtr&, json&, Timestamp) {
        // 客户端发来的 PONG，活跃时间已在 handleMessage 中更新
    };
}

// ==================== Redis 跨服消息回调 ====================
void ChatService::onRedisMessage(const std::string&, const std::string& message) {
    json js;
    try {
        js = json::parse(message);
    } catch (const json::parse_error& e) {
                LOG_ERROR("[Redis] JSON parse error: %s", e.what());
        return;
    }

    int msgid = js.value("msgid", 0);

    // ===== 一对一聊天（已有） =====
    if (msgid == ONE_CHAT_MSG) {
        int to_id = js.value("toid", 0);
        std::lock_guard<std::mutex> lock(onlineMtx_);
        auto it = onlineUsers_.find(to_id);
        if (it != onlineUsers_.end()) {
            it->second->send(js.dump() + "\n");
                        LOG_INFO("[Redis] cross-server forward to uid=%d", to_id);
        }
        return;
    }

    // ===== 阶段7新增：群组聊天跨服转发 =====
    if (msgid == GROUP_CHAT_MSG) {
        int groupid = js["groupid"].get<int>();

        auto mysql = ConnectionPool::instance()->getConnection();
        if (!mysql) return;

        auto members = GroupModel::queryMembers(mysql.get(), groupid);

        std::lock_guard<std::mutex> lock(onlineMtx_);
        for (int memberId : members) {
            auto it = onlineUsers_.find(memberId);
            if (it != onlineUsers_.end()) {
                it->second->send(js.dump() + "\n");
                LOG_INFO("[Redis] cross-server group forward to memberId=%d", memberId);
            }
        }
    }
}

// ==================== 断线处理 ====================
void ChatService::handleConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
                LOG_INFO("New connection: %s", conn->peerAddress().toIpPort().c_str());
    } else {
                LOG_INFO("Connection closed: %s", conn->peerAddress().toIpPort().c_str());
        std::lock_guard<std::mutex> lk(onlineMtx_);
        for (auto it = onlineUsers_.begin(); it != onlineUsers_.end(); ++it) {
            if (it->second == conn) {
                int uid = it->first;
                onlineUsers_.erase(it);
                {
                    std::lock_guard<std::mutex> lk2(
                        ChatService::instance()->activeMtx_);
                    ChatService::instance()->lastActiveTime_.erase(uid);
                }
                auto mysql = ConnectionPool::instance()->getConnection();
                if (mysql) {
                    char sql[128];
                    snprintf(sql, sizeof(sql),
                             "UPDATE User SET state='offline' WHERE id=%d", uid);
                    mysql->update(sql);
                    LOG_INFO("[DISCONNECT] user id=%d -> offline", uid);
                }
                break;
            }
        }
    }
}

MsgHandler ChatService::getHandler(int msgid) {
    auto it = handlers_.find(msgid);
    if (it != handlers_.end()) return it->second;
    return [](const TcpConnectionPtr&, json& js, Timestamp) {
                LOG_ERROR("[ERROR] Unknown msgid: %d", js["msgid"].get<int>());
    };
}

void ChatService::handleMessage(const TcpConnectionPtr& conn,
                                 Buffer* buf, Timestamp time) {
    std::string raw = buf->retrieveAllAsString();
    json js;
    try {
        js = json::parse(raw);
    } catch (const json::parse_error& e) {
                LOG_ERROR("[ERROR] JSON parse: %s", e.what());
        return;
    }
    int uid = getUidByConn(conn);
    if (uid != -1) {
        resetActiveTime(uid);
    }
    auto handler = getHandler(js["msgid"].get<int>());
    handler(conn, js, time);
}

// ========== 阶段7新增：通过连接反查 uid ==========
int ChatService::getUidByConn(const TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(onlineMtx_);
    for (const auto& kv : onlineUsers_) {
        if (kv.second == conn) return kv.first;
    }
    return -1;
}

// ========== 阶段7新增：群聊消息处理 ==========
void ChatService::handleGroupChat(const TcpConnectionPtr& conn, json& js) {
    int groupid = js.value("groupid", 0);
    std::string message = js.value("message", "");

    if (groupid <= 0 || message.empty() || message.size() > 5000) return;

    auto mysql = ConnectionPool::instance()->getConnection();
    if (!mysql) return;

    // 查询群成员
    auto members = GroupModel::queryMembers(mysql.get(), groupid);
    if (members.empty()) return;

    // 确保消息里有发送者信息
    int senderId = getUidByConn(conn);
    if (senderId == -1) return;
    js["id"] = senderId;

    // ① 本机在线成员：直接转发
    {
        std::lock_guard<std::mutex> lock(onlineMtx_);
        for (int memberId : members) {
            if (memberId == senderId) continue;  // 不发给自己
            auto it = onlineUsers_.find(memberId);
            if (it != onlineUsers_.end()) {
                it->second->send(js.dump() + "\n");
                LOG_INFO("[GROUP_CHAT] local forward to memberId=%d", memberId);
            }
        }
    }
    redis_.publish("chat_server", js.dump());

    // ③ 离线消息：写入所有不在线的群成员
    std::string escaped = mysql->escape(js.dump());
    {
        std::lock_guard<std::mutex> lock(onlineMtx_);
        for (int memberId : members) {
            if (memberId == senderId) continue;
            if (onlineUsers_.find(memberId) != onlineUsers_.end()) continue;
            // 不在线 → 写入离线消息
            char sql[8192];
            snprintf(sql, sizeof(sql),
                "INSERT INTO OfflineMessage (userid, message) VALUES (%d, '%s')",
                memberId, escaped.c_str());
            mysql->update(sql);
        }
    }
    LOG_INFO("[GROUP_CHAT] uid=%d to group %d: %s", senderId, groupid, message.c_str());
}
// ==================== 阶段8d：心跳机制 ====================

void ChatService::startHeartbeat(muduo::net::EventLoop* loop) {
    loop->runEvery(kHeartbeatInterval, [this]() {
        checkHeartbeat();
    });
    LOG_INFO("[HEARTBEAT] timer started (interval=%.0fs, timeout=%.0fs)",
             kHeartbeatInterval, kHeartbeatTimeout);
}

void ChatService::checkHeartbeat() {
    Timestamp now = Timestamp::now();
    std::vector<int> timeoutUids;

    // 第一步：找出所有超时的 uid（只锁 activeMtx_）
    {
        std::lock_guard<std::mutex> lock(activeMtx_);
        for (auto it = lastActiveTime_.begin(); it != lastActiveTime_.end(); ) {
            if (muduo::timeDifference(now, it->second) > kHeartbeatTimeout) {
                timeoutUids.push_back(it->first);
                it = lastActiveTime_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 第二步：逐个踢下线（不再持有 activeMtx_，避免死锁）
    for (int uid : timeoutUids) {
        TcpConnectionPtr conn;
        {
            std::lock_guard<std::mutex> lock(onlineMtx_);
            auto it = onlineUsers_.find(uid);
            if (it != onlineUsers_.end()) {
                conn = it->second;
                onlineUsers_.erase(it);
            }
        }

        if (conn) {
            // 更新 DB 状态为 offline
            auto mysql = ConnectionPool::instance()->getConnection();
            if (mysql) {
                char sql[128];
                snprintf(sql, sizeof(sql),
                         "UPDATE User SET state='offline' WHERE id=%d", uid);
                mysql->update(sql);
            }

            LOG_INFO("[HEARTBEAT] uid=%d timeout, kicking off", uid);
            conn->shutdown();   // muduo 线程安全
        }
    }
}

void ChatService::resetActiveTime(int uid) {
    std::lock_guard<std::mutex> lock(activeMtx_);
    lastActiveTime_[uid] = Timestamp::now();
}