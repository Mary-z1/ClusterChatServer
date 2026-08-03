
# 集群聊天服务器 (Cluster Chat Server)

[![C++](https://img.shields.io/badge/language-C++14-blue)](https://en.cppreference.com/)
[![muduo](https://img.shields.io/badge/network-muduo-brightgreen)](https://github.com/chenshuo/muduo)
[![MySQL](https://img.shields.io/badge/db-MySQL%208.0-orange)](https://www.mysql.com/)
[![Redis](https://img.shields.io/badge/cache-Redis%207.0-red)](https://redis.io/)
[![Nginx](https://img.shields.io/badge/proxy-Nginx%20stream-green)](https://nginx.org/)

> 🚧 开发完成，待部署验证

基于 muduo 网络库的 **高性能分布式聊天服务器**，支持一对一聊天、群组聊天、好友管理、离线消息、跨服务器通信和 Nginx 负载均衡。

---

## 功能特性

-  **用户系统** — 注册 / 登录 / 登出，SHA256 密码哈希
-  **一对一聊天** — 实时转发 + 离线消息缓存 + 上线自动推送
-  **好友管理** — 添加好友 / 查看好友列表
-  **群组聊天** — 创建群组 / 加入群组 / 群内广播
-  **跨服通信** — Redis Pub/Sub 实现多节点消息同步
-  **负载均衡** — Nginx TCP stream 四层代理
-  **MySQL 连接池** — RAII 自动归还连接
-  **异步日志** — 双缓冲队列 + 后台线程写入
-  **心跳检测** — 客户端心跳保活，超时自动踢下线

---

---

## 技术栈

| 类别 | 选型 |
|------|------|
| 语言 | C++14 |
| 网络库 | muduo |
| 数据库 | MySQL 8.0 |
| 缓存 / 消息中间件 | Redis 7.0 (Pub/Sub) |
| 负载均衡 | Nginx stream |
| 序列化 | JSON (nlohmann/json) |
| 构建系统 | CMake |
| 环境 | Ubuntu 22.04 |

---

## 快速开始

### 环境要求

- Ubuntu 22.04
- GCC ≥ 7（支持 C++14）
- CMake ≥ 3.10

### 1. 安装系统依赖

```bash
sudo apt update && sudo apt install -y \
    g++ cmake make \
    libmysqlclient-dev libhiredis-dev \
    mysql-server redis-server nginx
```
### 2. 编译安装 muduo
```
git clone https://github.com/chenshuo/muduo.git
cd muduo
./build.sh && sudo ./build.sh install
cd ..
```
### 3. 编译本项目
~~~
git clone https://github.com/Mary-z1/ClusterChatServer.git
cd ClusterChatServer
mkdir build && cd build
cmake .. && make -j$(nproc)
~~~
### 4. 配置
~~~
vim conf/server.conf    # 修改 MySQL 密码等配置
~~~
### 5. 启动服务
~~~
sudo service mysql start
sudo service redis-server start
./bin/chat_server
~~~
## 关键设计

### 消息路由

客户端 JSON → Buffer → ChatService::handleMessage()

     json::parse() 解析

     取 msgid，查 unordered_map<int, MsgHandler>

     调用对应 Handler（lambda 注册在构造函数中）

### 连接池

~~~

单例 + queue<MySQL*> + mutex + semaphore

获取: semaphore.wait() → pop front → 返回 shared_ptr<MySQL>

归还: shared_ptr 析构 → 自定义删除器 → push back + semaphore.post()

防御: tryReconnect() 自动重连 (5s 冷却), getConnection() 失败返回 nullptr
~~~

### 跨服通信

~~~

ClientA → ChatServer1 → Redis Publish (channel: "chat_server")

     ChatServer2 收到消息

     查在线用户 map → 在线 → 直接转发

     不在线 → 写入 OfflineMessage 表
   ~~~

### 异步日志

~~~

单例 + 后台线程 + queue + mutex + condition_variable

LOG_INFO/LOG_ERROR 宏 → push 队列 → notify_one()

后台线程: wait() → pop → fwrite + fflush

退出时 shutdown() 刷空队列
~~~


