#pragma once

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <memory>
#include <atomic>
#include <chrono>

class MySQL {
public:
    MySQL();
    ~MySQL();

    bool connect(const std::string& host, int port,
                 const std::string& user, const std::string& password,
                 const std::string& dbname);
    bool update(const std::string& sql);
    MYSQL_RES* query(const std::string& sql);
    MYSQL* getConnection();
    void setAlive(bool alive) { alive_ = alive; }
    bool isAlive() const { return alive_; }
    std::string escape(const std::string& str);

private:
    MYSQL* conn_;
    bool alive_;
};

class ConnectionPool {
public:
    static ConnectionPool* instance();
    std::shared_ptr<MySQL> getConnection();

private:
    ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    void initPool(int initialSize);
    void produceConnection();
    bool tryReconnect();

    std::queue<MySQL*> pool_;
    std::mutex mtx_;
    sem_t sem_;
    int maxSize_;
    std::atomic<bool> initOk_;
    std::string host_;
    int port_;
    std::string user_;
    std::string password_;
    std::string dbname_;
    std::chrono::steady_clock::time_point lastReconnectTime_;
    static constexpr int kReconnectIntervalSec = 5;
};
