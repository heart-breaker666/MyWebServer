#include "../include/ConnectionPool.h"

#include "../include/Log.h"

ConnectionPool& ConnectionPool::GetInstance() {
    // C++11 magic static：首次调用时构造，标准保证其线程安全
    static ConnectionPool instance;
    return instance;
}

ConnectionPool::ConnectionPool()
    : max_conn_(0), free_conn_(0), close_log_(0) {}

ConnectionPool::~ConnectionPool() {
    DestroyPool();
}

void ConnectionPool::Init(const std::string& url, const std::string& user,
                          const std::string& password,
                          const std::string& database_name, int port,
                          int max_conn, int close_log) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    // 支持重复初始化：先销毁旧连接
    DestroyPoolLocked();

    close_log_ = close_log;
    max_conn_ = max_conn;
    for (int i = 0; i < max_conn; ++i) {
        MYSQL* conn = mysql_init(nullptr);
        if (conn == nullptr) {
            if (!close_log_) {
                LOG_ERROR("ConnectionPool: mysql_init failed");
            }
            break;
        }
        if (mysql_real_connect(conn, url.c_str(), user.c_str(),
                               password.c_str(), database_name.c_str(), port,
                               nullptr, 0) == nullptr) {
            if (!close_log_) {
                LOG_ERROR("ConnectionPool: connect failed: %s",
                          mysql_error(conn));
            }
            mysql_close(conn);
            break;
        }
        conn_queue_.push(conn);
        ++free_conn_;
    }
}

MYSQL* ConnectionPool::GetConnection() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    // 连接池为空时阻塞等待，直到有连接被归还
    queue_cv_.wait(lock, [this] { return !conn_queue_.empty(); });
    MYSQL* conn = conn_queue_.front();
    conn_queue_.pop();
    --free_conn_;
    return conn;
}

void ConnectionPool::ReleaseConnection(MYSQL* conn) {
    if (conn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    conn_queue_.push(conn);
    ++free_conn_;
    queue_cv_.notify_one();
}

void ConnectionPool::DestroyPool() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    DestroyPoolLocked();
}

void ConnectionPool::DestroyPoolLocked() {
    while (!conn_queue_.empty()) {
        MYSQL* conn = conn_queue_.front();
        conn_queue_.pop();
        mysql_close(conn);
    }
    free_conn_ = 0;
    max_conn_ = 0;
}

int ConnectionPool::GetFreeConnCount() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return free_conn_;
}

ConnectionRAII::ConnectionRAII(ConnectionPool& pool)
    : pool_(pool), conn_(pool_.GetConnection()) {}

ConnectionRAII::~ConnectionRAII() {
    pool_.ReleaseConnection(conn_);
}

MYSQL* ConnectionRAII::Get() const {
    return conn_;
}
