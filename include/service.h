#ifndef SERVICE_H
#define SERVICE_H

#include "public.h"
#include "redis.h"
#include <unordered_map>
#include <mutex>

class ChatService {
public:
    static ChatService* instance();
    MsgHandler getHandler(int msgid);
    void handleConnection(const TcpConnectionPtr& conn);
    void handleMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time);

    // 在线用户与锁
    std::unordered_map<int, TcpConnectionPtr> onlineUsers_;
    std::mutex onlineMtx_;

    // Redis 操作（跨服通信）
    Redis redis_;

private:
    ChatService();
    std::unordered_map<int, MsgHandler> handlers_;

    // Redis 订阅回调
    void onRedisMessage(const std::string& channel, const std::string& message);

    // ========== 阶段7新增：辅助方法 ==========
    // 通过 conn 反查 uid（-1 表示未登录）
    int getUidByConn(const TcpConnectionPtr& conn);

    // 处理群聊消息的核心逻辑（本机转发 + Redis 发布 + 离线存储）
    void handleGroupChat(const TcpConnectionPtr& conn, json& js);
};

#endif