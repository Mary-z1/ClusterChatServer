#include "server.h"
#include "service.h"
#include "logger.h"

ChatServer::ChatServer(EventLoop* loop, const InetAddress& addr, const std::string& name)
    : server_(loop, addr, name)
{
    server_.setConnectionCallback(
        std::bind(&ChatServer::onConnection, this,
                  std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&ChatServer::onMessage, this,
                  std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

ChatServer::~ChatServer() {
    // 空实现：TcpServer 的析构由下面的 stop() 在 loop 线程中完成
    // 如果没调 stop() 就析构 → 说明 loop 还没退出 → TcpServer 析构 OK
}

void ChatServer::start() {
    server_.start();
}

void ChatServer::stop() {
    if (stopped_) return;
    stopped_ = true;
    // 通知 muduo 退出 acceptor 循环，关闭所有连接
    // getLoop() 返回 acceptor loop（即主线程的 loop）
    server_.getLoop()->quit();
}

void ChatServer::onConnection(const TcpConnectionPtr& conn) {
    ChatService::instance()->handleConnection(conn);
}

void ChatServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
    ChatService::instance()->handleMessage(conn, buf, time);
}