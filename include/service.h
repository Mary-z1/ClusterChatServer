#ifndef SERVICE_H
#define SERVICE_H

#include "public.h"
#include "redis.h"
#include <unordered_map>
#include <mutex>
#include <muduo/net/EventLoop.h>

class ChatService {
public:
    static ChatService* instance();
    MsgHandler getHandler(int msgid);
    void handleConnection(const TcpConnectionPtr& conn);
    void handleMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time);

    // ========== 阶段8d：心跳机制（public，供 main 调用）==========
    void startHeartbeat(muduo::net::EventLoop* loop);

    // 在线用户与锁
    std::unordered_map<int, TcpConnectionPtr> onlineUsers_;
    std::mutex onlineMtx_;

    // Redis 操作（跨服通信）
    Redis redis_;

private:
    ChatService();
    std::unordered_map<int, MsgHandler> handlers_;

    void onRedisMessage(const std::string& channel, const std::string& message);
    int getUidByConn(const TcpConnectionPtr& conn);
    void handleGroupChat(const TcpConnectionPtr& conn, json& js);

    // 心跳内部方法（private）
    void checkHeartbeat();
    void resetActiveTime(int uid);

    std::unordered_map<int, muduo::Timestamp> lastActiveTime_;
    std::mutex activeMtx_;
    static constexpr double kHeartbeatInterval = 10.0;
    static constexpr double kHeartbeatTimeout  = 90.0;
};

#endif