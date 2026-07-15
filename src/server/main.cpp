// src/server/main.cpp
#include "server.h"
#include "logger.h"
#include <iostream>
#include <cstdlib>
#include <csignal>

// 全局指针，信号处理用
static EventLoop* g_loop = nullptr;

void signalHandler(int sig)
{
    std::cout << "\n[main] Received signal " << sig << ", shutting down..." << std::endl;
    if (g_loop) {
        g_loop->quit();  // muduo 安全退出事件循环
    }
}

int main(int argc, char* argv[])
{
    // === 初始化自定义异步日志 ===
    // 使用项目根目录下的 logs/（build 目录的上一级）
    Logger::instance()->init("../logs/chat_server.log", Logger::INFO);

    uint16_t port = 6000;  // 默认端口

    if (argc >= 2) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) {
            port = static_cast<uint16_t>(p);
        } else {
            LOG_ERROR("Invalid port: %s (must be 1-65535)", argv[1]);
            Logger::instance()->shutdown();
            return 1;
        }
    }

    // === 注册信号处理 ===
    signal(SIGINT,  signalHandler);   // Ctrl+C
    signal(SIGTERM, signalHandler);   // kill 默认信号

    EventLoop loop;
    g_loop = &loop;
    InetAddress listenAddr(port);
    ChatServer server(&loop, listenAddr, "ChatServer");

    LOG_INFO("ChatServer started on port %d", port);
    server.start();
    loop.loop();   // 阻塞直到 quit()

    LOG_INFO("ChatServer stopped.");
    Logger::instance()->shutdown();
    return 0;
}