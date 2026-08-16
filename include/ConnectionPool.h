#pragma once

#include <mysql/mysql.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

// 数据库连接池单例类（懒汉模式，C++11 magic static）。
// 参考 TinyWebServer 的 sql_connection_pool 设计：
//   - 初始化时一次性创建 max_conn 条 MySQL 连接放入队列
//   - 获取连接：队列为空时阻塞等待（条件变量，行为等价于信号量）
//   - 释放连接：归还队列并唤醒等待线程
//   - 通过 ConnectionRAII 封装实现连接的自动获取与归还，避免泄漏
class ConnectionPool {
public:
    // 获取全局唯一实例（懒汉式，线程安全）
    static ConnectionPool& GetInstance();

    // 初始化连接池。
    // url:           MySQL 服务器地址
    // user:          登录用户名
    // password:      登录密码
    // database_name: 数据库名
    // port:          MySQL 端口号
    // max_conn:      连接池最大连接数
    // close_log:     1 关闭日志，0 开启（为 1 时初始化失败不打印日志）
    void Init(const std::string& url, const std::string& user,
              const std::string& password, const std::string& database_name,
              int port, int max_conn, int close_log);

    // 从连接池获取一条连接，连接池为空时阻塞等待
    MYSQL* GetConnection();

    // 归还一条连接至连接池
    void ReleaseConnection(MYSQL* conn);

    // 销毁连接池：关闭所有连接并清空队列
    void DestroyPool();

    // 当前空闲连接数
    int GetFreeConnCount() const;

private:
    ConnectionPool();
    ~ConnectionPool();

    // 禁用拷贝构造与赋值
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // 销毁连接池的具体实现（调用方须已持有 queue_mutex_）
    void DestroyPoolLocked();

    std::queue<MYSQL*> conn_queue_;     // 连接池队列（存放空闲连接）
    mutable std::mutex queue_mutex_;    // 保护连接队列
    std::condition_variable queue_cv_;  // 队列非空条件变量
    int max_conn_;                      // 连接池最大连接数
    int free_conn_;                     // 当前空闲连接数
    int close_log_;                     // 是否关闭日志：1 关闭，0 开启
};

// RAII 封装：构造时从连接池获取连接，析构时自动归还，保证连接不泄漏
class ConnectionRAII {
public:
    // 从连接池获取一条连接并持有
    explicit ConnectionRAII(ConnectionPool& pool);

    ~ConnectionRAII();

    // 获取持有的 MySQL 连接
    MYSQL* Get() const;

private:
    // 禁用拷贝构造与赋值
    ConnectionRAII(const ConnectionRAII&) = delete;
    ConnectionRAII& operator=(const ConnectionRAII&) = delete;

    ConnectionPool& pool_;  // 所属连接池
    MYSQL* conn_;           // 持有的连接
};
