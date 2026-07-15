#ifndef REDIS_H
#define REDIS_H

#include <hiredis/hiredis.h>
#include <thread>
#include <functional>
#include <string>
#include <atomic>

class Redis {
public:
    Redis();
    ~Redis();

    // 连接 Redis，返回是否成功
    bool connect(const std::string& host = "127.0.0.1", int port = 6379);

    // 发布消息到频道
    bool publish(const std::string& channel, const std::string& message);

    // 订阅频道（启动独立线程阻塞接收）
    void subscribe(const std::string& channel);

    // 注册收到消息时的回调
    void setNotifyCallback(std::function<void(const std::string& channel,
                                               const std::string& message)> cb);

private:
    void observerChannel();   // 订阅线程主循环

    redisContext* publishCtx_;    // 发布用（命令模式，线程安全）
    redisContext* subscribeCtx_;  // 订阅用（阻塞模式，独占线程）
    std::function<void(const std::string&, const std::string&)> notifyCb_;
    std::thread subThread_;
    std::atomic<bool> stopped_{false};
};

#endif