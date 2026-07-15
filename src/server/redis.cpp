#include "redis.h"
#include "logger.h"
#include <iostream>

Redis::Redis() : publishCtx_(nullptr), subscribeCtx_(nullptr) {}

Redis::~Redis() {
    stopped_ = true;
    // 向 subscribe 连接发送 UNSUBSCRIBE 来唤醒阻塞的 redisGetReply
    if (subscribeCtx_) {
        redisReply* reply = static_cast<redisReply*>(
            redisCommand(subscribeCtx_, "UNSUBSCRIBE chat_server"));
        if (reply) freeReplyObject(reply);
    }
    if (subThread_.joinable()) subThread_.join();
    if (publishCtx_) redisFree(publishCtx_);
    if (subscribeCtx_) redisFree(subscribeCtx_);
}

bool Redis::connect(const std::string& host, int port) {
    publishCtx_ = redisConnect(host.c_str(), port);
    if (!publishCtx_ || publishCtx_->err) {
        LOG_ERROR("[Redis] publish connect failed: %s", publishCtx_->errstr);
        return false;
    }

    subscribeCtx_ = redisConnect(host.c_str(), port);
    if (!subscribeCtx_ || subscribeCtx_->err) {
        LOG_ERROR("[Redis] subscribe connect failed: %s", subscribeCtx_->errstr);
    }

        LOG_INFO("[Redis] connected to %s:%d", host.c_str(), port);
    return true;
}

bool Redis::publish(const std::string& channel, const std::string& message) {
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(publishCtx_, "PUBLISH %s %s",
                     channel.c_str(), message.c_str()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return ok;
}

void Redis::subscribe(const std::string& channel) {
    subThread_ = std::thread([this, channel]() {
        // 发送 SUBSCRIBE 命令
        redisReply* reply = static_cast<redisReply*>(
            redisCommand(subscribeCtx_, "SUBSCRIBE %s", channel.c_str()));
        if (reply) freeReplyObject(reply);

        // 循环接收消息（redisGetReply 会阻塞）
        while (!stopped_) {
            redisReply* msg = nullptr;
            int ret = redisGetReply(subscribeCtx_, (void**)&msg);
            if (ret != REDIS_OK || !msg) break;

            if (msg->type == REDIS_REPLY_ARRAY && msg->elements >= 3) {
                std::string ch(msg->element[1]->str, msg->element[1]->len);
                std::string payload(msg->element[2]->str, msg->element[2]->len);
                if (notifyCb_) notifyCb_(ch, payload);
            }
            freeReplyObject(msg);
        }
    });
}

void Redis::setNotifyCallback(std::function<void(const std::string&,
                                                  const std::string&)> cb) {
    notifyCb_ = std::move(cb);
}