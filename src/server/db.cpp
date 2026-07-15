#include "db.h"
#include "logger.h"
#include "config.h" 
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
    : initOk_(false)
{
    auto* cfg = Config::instance();
    host_     = cfg->getString("mysql", "host",     "127.0.0.1");
    port_     = cfg->getInt(   "mysql", "port",     3306);
    user_     = cfg->getString("mysql", "user",     "chatuser");
    password_ = cfg->getString("mysql", "password", "123456");
    dbname_   = cfg->getString("mysql", "db",       "chat");
    maxSize_  = cfg->getInt(   "mysql", "poolSize", 4);
    int initSize = maxSize_;

    sem_init(&sem_, 0, maxSize_);
    initPool(initSize);
}
// ========== 初始化连接池 ==========
void ConnectionPool::initPool(int size) {
    for (int i = 0; i < size; ++i) {
        MySQL* mysql = new MySQL();
        if (mysql->connect(host_, port_, user_, password_, dbname_)) {
            std::lock_guard<std::mutex> lock(mtx_);
            pool_.push(mysql);
            sem_post(&sem_);
        } else {
            delete mysql;
        }
    }
    if (!pool_.empty()) {
        initOk_.store(true);
    }
}

// ========== 获取连接（信号量阻塞等待 + 重连） ==========
std::shared_ptr<MySQL> ConnectionPool::getConnection() {
    // 如果池子坏了，尝试重连
    if (!initOk_.load()) {
        tryReconnect();
        if (!initOk_.load()) return nullptr;
    }

    sem_wait(&sem_);
    std::lock_guard<std::mutex> lock(mtx_);
    MySQL* raw = pool_.front();
    pool_.pop();

    // 检查连接是否存活，不存活则重连
    if (!raw->isAlive()) {
        LOG_WARN("[Pool] dead connection, reconnecting...");
        if (!raw->connect(host_, port_, user_, password_, dbname_)) {
            delete raw;
            LOG_ERROR("[Pool] reconnect failed, dropping connection");
            return nullptr;
        }
    }

    return std::shared_ptr<MySQL>(raw, [this](MySQL* p) {
        if (p) {
            std::lock_guard<std::mutex> lock(mtx_);
            pool_.push(p);
            sem_post(&sem_);
        }
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
