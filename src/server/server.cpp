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

void ChatServer::start() {
    server_.start();
}

void ChatServer::onConnection(const TcpConnectionPtr& conn) {
    ChatService::instance()->handleConnection(conn);
}

void ChatServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp time) {
    ChatService::instance()->handleMessage(conn, buf, time);
}
