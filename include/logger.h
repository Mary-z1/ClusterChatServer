#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <atomic>

class Logger {
public:
    enum Level { DEBUG = 0, INFO, WARN, ERROR, FATAL };

    static Logger* instance();

    // 初始化：指定日志文件路径、最低输出级别、单文件最大大小
    void init(const std::string& logPath = "logs/chat_server.log",
              Level minLevel = INFO,
              size_t maxFileSize = 100 * 1024 * 1024);

    // 写日志（内部使用，建议用宏）
    void log(Level level, const char* file, int line,
             const char* fmt, ...);

    // 关闭后台线程，flush 剩余日志
    void shutdown();

private:
    Logger() = default;
    ~Logger();

    void run();                          // 后台写盘线程
    static const char* levelStr(Level lv);
    void rollFile();                     // 日志滚动
    std::string timestamp();             // 时间戳生成

    struct LogMsg {
        Level level;
        std::string text;
    };

    std::queue<LogMsg> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread thread_;
    std::ofstream file_;
    std::string logPath_;
    Level minLevel_ = INFO;
    size_t maxFileSize_ = 100 * 1024 * 1024;
    size_t currentSize_ = 0;
    bool running_ = false;
    std::atomic<bool> ready_{false};
};

// ========== 便捷宏 ==========
#define LOG_DEBUG(fmt, ...) \
    Logger::instance()->log(Logger::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    Logger::instance()->log(Logger::INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    Logger::instance()->log(Logger::WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    Logger::instance()->log(Logger::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) \
    Logger::instance()->log(Logger::FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
