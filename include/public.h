#pragma once

#include <muduo/net/TcpConnection.h>
#include <muduo/net/Buffer.h>
#include <muduo/base/Timestamp.h>
#include <functional>
#include "json.hpp"

// 类型别名
using json = nlohmann::json;
using TcpConnectionPtr = muduo::net::TcpConnectionPtr;
using Buffer = muduo::net::Buffer;
using Timestamp = muduo::Timestamp;
using MsgHandler = std::function<void(const TcpConnectionPtr&, json&, Timestamp)>;

// 消息类型枚举
enum EnMsgType {
    LOGIN_MSG = 1,       // 登录
    LOGIN_MSG_ACK,       // 登录响应       (=2)
    REG_MSG,             // 注册           (=3)
    REG_MSG_ACK,         // 注册响应       (=4)
    ONE_CHAT_MSG,        // 一对一聊天     (=5)
    ADD_FRIEND_MSG,      // 添加好友       (=6)
    CREATE_GROUP_MSG,    // 创建群组       (=7)
    ADD_GROUP_MSG,       // 加入群组       (=8)
    GROUP_CHAT_MSG,      // 群组聊天       (=9)
    ADD_FRIEND_MSG_ACK,  // 添加好友响应   (=10)
    CREATE_GROUP_MSG_ACK,// 创建群组响应   (=11)
    ADD_GROUP_MSG_ACK,   // 加入群组响应   (=12)
    GROUP_CHAT_MSG_ACK,  // 群组聊天响应   (=13)
    PING_MSG,            // 心跳请求       (=14)
    PONG_MSG,            // 心跳响应       (=15)
    DEL_FRIEND_MSG,      // 删除好友       (=16)
    DEL_FRIEND_MSG_ACK,  // 删除好友响应   (=17)
    QUIT_GROUP_MSG,      // 退出群组       (=18)
    QUIT_GROUP_MSG_ACK,  // 退出群组响应   (=19)
    FRIEND_STATUS_REQ,   // 好友状态查询请求 (=20)
    FRIEND_STATUS_ACK,   // 好友状态查询响应 (=21)
    FRIEND_LIST_MSG,     // 好友列表推送   (=22)
    GROUP_LIST_MSG,      // 群组列表推送   (=23)
};
