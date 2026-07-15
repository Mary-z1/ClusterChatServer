#include "db.h"
#include "logger.h"
#include <iostream>

MySQL::MySQL() : conn_(nullptr), alive_(false) {}

MySQL::~MySQL() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

bool MySQL::connect(const std::string& host, int port,
                    const std::string& user, const std::string& password,
                    const std::string& dbname) {
    conn_ = mysql_init(nullptr);
    if (!conn_) {
        LOG_ERROR("[DB] mysql_init failed");
        return false;
    }

    if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), password.c_str(),
                            dbname.c_str(), port, nullptr, 0)) {
        LOG_ERROR("[DB] connect failed: %s", mysql_error(conn_));
        return false;
    }
    alive_ = true;
    return true;
}

bool MySQL::update(const std::string& sql) {
    if (mysql_query(conn_, sql.c_str())) {
        LOG_ERROR("[DB] update failed: %s", mysql_error(conn_));
        return false;
    }
    return true;
}

MYSQL_RES* MySQL::query(const std::string& sql) {
    if (mysql_query(conn_, sql.c_str())) {
        LOG_ERROR("[DB] query failed: %s", mysql_error(conn_));
        return nullptr;
    }
    return mysql_store_result(conn_);
}

MYSQL* MySQL::getConnection() { return conn_; }

// ============ ConnectionPool ============

ConnectionPool* ConnectionPool::instance() {
    static ConnectionPool pool;
    return &pool;
}

ConnectionPool::ConnectionPool()
    : maxSize_(8), initOk_(false),
      host_("127.0.0.1"), port_(3306),
      user_("chatuser"), password_("123456"),
      dbname_("chat") {
    sem_init(&sem_, 0, maxSize_);
    initPool(4);
}

void ConnectionPool::initPool(int initialSize) {
    for (int i = 0; i < initialSize; i++) {
        produceConnection();
    }
    // 至少有一个连接成功才算初始化成功
    std::lock_guard<std::mutex> lock(mtx_);
    initOk_ = !pool_.empty();
    if (!initOk_) {
        LOG_ERROR("[Pool] FATAL: no connection established!");
    }
}

void ConnectionPool::produceConnection() {
    MySQL* mysql = new MySQL();
    if (mysql->connect(host_, port_, user_, password_, dbname_)) {
        std::lock_guard<std::mutex> lock(mtx_);
        pool_.push(mysql);
        sem_post(&sem_);
        LOG_INFO("[Pool] connection OK, pool size: %zu", pool_.size());
    } else {
        delete mysql;
    }
}

std::shared_ptr<MySQL> ConnectionPool::getConnection() {
    if (!initOk_) tryReconnect();// 尝试重连
    // 初始化失败直接返回空
    if (!initOk_) {
        LOG_ERROR("[Pool] not initialized, cannot get connection");
        return nullptr;
    }
    sem_wait(&sem_);
    std::lock_guard<std::mutex> lock(mtx_);
    if (pool_.empty()) {
        // 防御：信号量计数异常时
        sem_post(&sem_);  // 归还信号量
        return nullptr;
    }
    MySQL* mysql = pool_.front();
    pool_.pop();
    return std::shared_ptr<MySQL>(mysql, [this](MySQL* conn) {
        std::lock_guard<std::mutex> lock(mtx_);
        pool_.push(conn);
        sem_post(&sem_);
    });
}
// ========== 自动重连 ==========
bool ConnectionPool::tryReconnect() {
    auto now = std::chrono::steady_clock::now();

    // 冷却检查
    if (lastReconnectTime_.time_since_epoch().count() != 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - lastReconnectTime_).count();
        if (elapsed < kReconnectIntervalSec) {
            return false;
        }
    }
    lastReconnectTime_ = now;

    LOG_INFO("[Pool] attempting reconnect...");

    // 尝试一根测试连接
    MYSQL* test = mysql_init(nullptr);
    if (!test) return false;

    if (!mysql_real_connect(test, host_.c_str(), user_.c_str(),
                            password_.c_str(), dbname_.c_str(),
                            port_, nullptr, 0)) {
        LOG_ERROR("[Pool] reconnect probe failed: %s", mysql_error(test));
        mysql_close(test);
        return false;
    }
    mysql_close(test);  // 探针成功，关闭它

    // 重建连接池
    int success = 0;
    for (int i = 0; i < maxSize_; i++) {
        MySQL* mysql = new MySQL();
        if (mysql->connect(host_, port_, user_, password_, dbname_)) {
            std::lock_guard<std::mutex> lock(mtx_);
            pool_.push(mysql);
            sem_post(&sem_);
            success++;
        } else {
            delete mysql;
        }
    }

    if (success > 0) {
        initOk_.store(true);
        LOG_INFO("[Pool] reconnect OK! %d connections", success);
        return true;
    }

    LOG_ERROR("[Pool] reconnect failed, retry in %ds", kReconnectIntervalSec);
    return false;
}

// ========== SQL 注入防护 ==========
std::string MySQL::escape(const std::string& str) {
    if (!conn_ || str.empty()) return str;
    // mysql_real_escape_string 最坏情况每个字符都需要转义 → 2*len+1
    std::string escaped(str.size() * 2 + 1, '\0');
    size_t new_len = mysql_real_escape_string(
        conn_, &escaped[0], str.c_str(), str.size());
    escaped.resize(new_len);
    return escaped;
}
