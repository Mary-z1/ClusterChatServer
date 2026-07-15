#include "logger.h"
#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>
#include <atomic>
#include <iostream>

Logger* Logger::instance() {
    static Logger logger;
    return &logger;
}

Logger::~Logger() {
    shutdown();
}

void Logger::init(const std::string& logPath, Level minLevel,
                  size_t maxFileSize) {
    logPath_     = logPath;
    minLevel_    = minLevel;
    maxFileSize_ = maxFileSize;

    // 确保日志目录存在
    size_t pos = logPath_.find_last_of('/');
    if (pos != std::string::npos) {
        std::string dir = logPath_.substr(0, pos);
        mkdir(dir.c_str(), 0755);
    }

    // 打开文件（append 模式）
    file_.open(logPath_, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "[Logger] failed to open: " << logPath_ << std::endl;
        return;
    }
    // 记录当前文件大小
    file_.seekp(0, std::ios::end);
    currentSize_ = file_.tellp();

    // 启动后台线程
    running_ = true;
    thread_  = std::thread(&Logger::run, this);

    // ✅ 修复1：等待后台线程真正进入 wait 状态
    while (!ready_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // ✅ 修复2：直接写初始化日志，不经过异步队列（避免竞态）
    {
        std::ostringstream oss;
        oss << timestamp() << " [INFO ] [logger.cpp:0] "
            << "===== Logger initialized, file: " << logPath_ << " =====\n";
        std::string line = oss.str();
        std::cout << line;
        std::cout.flush();
        if (file_.is_open()) {
            file_ << line;
            file_.flush();
            currentSize_ += line.size();
        }
    }
}

void Logger::shutdown() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void Logger::log(Level level, const char* file, int line,
                 const char* fmt, ...) {
    if (level < minLevel_) return;

    // 格式化用户消息
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 组装完整日志行
    std::ostringstream oss;
    oss << timestamp() << " "
        << "[" << levelStr(level) << "] "
        << "[" << file << ":" << line << "] "
        << buf << "\n";

    // 同时输出到控制台
    if (level >= WARN) {
        std::cerr << oss.str();
    } else {
        std::cout << oss.str();
    }
    std::cout.flush();

    // 推入队列
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push({level, oss.str()});
    }
    cv_.notify_one();
}

void Logger::run() {
    ready_.store(true, std::memory_order_release);  // ✅ 通知 init：我已就绪

    while (running_) {
        std::unique_lock<std::mutex> lock(mtx_);
        // ✅ 修复3：1秒超时兜底，即使 notify 丢失也能醒来处理
        cv_.wait_for(lock, std::chrono::seconds(1), [this] {
            return !queue_.empty() || !running_;
        });

        // 批量取出，减少锁竞争
        while (!queue_.empty()) {
            LogMsg msg = std::move(queue_.front());
            queue_.pop();
            lock.unlock();

            if (file_.is_open()) {
                file_ << msg.text;
                currentSize_ += msg.text.size();
            }

            if (currentSize_ >= maxFileSize_) {
                rollFile();
            }

            lock.lock();
        }
    }

    // shutdown 时清空残留
    while (!queue_.empty()) {
        if (file_.is_open()) {
            file_ << queue_.front().text;
        }
        queue_.pop();
    }
    if (file_.is_open()) {
        file_.flush();
    }
}

void Logger::rollFile() {
    if (!file_.is_open()) return;
    file_.flush();
    file_.close();

    std::string backup = logPath_ + ".1";
    rename(logPath_.c_str(), backup.c_str());

    file_.open(logPath_, std::ios::app);
    currentSize_ = 0;

    if (!file_.is_open()) {
        std::cerr << "[Logger] roll failed: " << logPath_ << std::endl;
    }
}

std::string Logger::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                   now.time_since_epoch()) % 1000000;
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&tt, &tm);

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, us.count());
    return buf;
}

const char* Logger::levelStr(Level lv) {
    switch (lv) {
        case DEBUG: return "DEBUG";
        case INFO:  return "INFO ";
        case WARN:  return "WARN ";
        case ERROR: return "ERROR";
        case FATAL: return "FATAL";
    }
    return "????";
}
