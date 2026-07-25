// src/server/main.cpp
#include "server.h"
#include "logger.h"
#include "config.h"   
#include "service.h"   
#include "db.h"   // ConnectionPool    
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

static EventLoop* g_loop = nullptr;
static std::atomic<bool> g_shuttingDown{false};
static ChatServer* g_server = nullptr;

void signalHandler(int sig)
{
    std::cout << "\n[main] Received signal " << sig << ", shutting down..." << std::endl;
    if (g_server) {
        g_server->stop();   // ← 这会在 loop 线程中 quit
    }
}

int main(int argc, char* argv[])
{
    // === 1. 加载配置文件（自动适配运行目录）===
    static const char* kConfigPaths[] = {
        "conf/server.conf",       // 从项目根目录运行
        "../conf/server.conf",    // 从 build/ 运行
    };
    bool loaded = false;
    for (const auto* p : kConfigPaths) {
        if (Config::instance()->load(p)) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        std::cerr << "[main] FATAL: failed to load config (tried conf/ and ../conf/)" << std::endl;
        return 1;
    }
    auto* cfg = Config::instance();

    // === 2. 初始化日志（路径从配置读取）===
    std::string logPath = cfg->getString("log", "path", "../logs/chat_server.log");
    std::string logLevel = cfg->getString("log", "level", "INFO");
    Logger::Level level = Logger::INFO;
    if (logLevel == "DEBUG") level = Logger::DEBUG;
    else if (logLevel == "WARN")  level = Logger::WARN;
    else if (logLevel == "ERROR") level = Logger::ERROR;
    else if (logLevel == "FATAL") level = Logger::FATAL;
    Logger::instance()->init(logPath, level);

    // === 3. 端口：优先命令行参数，其次配置文件 ===
    uint16_t port = static_cast<uint16_t>(cfg->getInt("server", "port", 6000));
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

    // === 4. 信号处理 ===
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    EventLoop loop;
    g_loop = &loop;
    InetAddress listenAddr(port);
    ChatServer server(&loop, listenAddr, "ChatServer");
    g_server = &server;

    LOG_INFO("ChatServer started on port %d", port);
    // ⭐ 预热：提前初始化 ChatService + ConnectionPool
    // 避免第一个请求时阻塞 muduo IO 线程
    ChatService::instance();
    ConnectionPool::instance();
    server.start();
    ChatService::instance()->startHeartbeat(&loop);
    loop.loop();

    LOG_INFO("ChatServer stopped.");
    Logger::instance()->shutdown();
    // ⚠️ 临时：强制退出，绕过 muduo TcpServer 析构阻塞
    // TODO: 后续添加 ChatServer::stop() 优雅清理
    _exit(0);
}