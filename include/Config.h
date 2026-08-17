#pragma once

class Config {
public:
    Config();
    ~Config() = default;
    void ParseArg(int argc, char* argv[]);

    // 监听端口号
    int GetPort() const;

    // 日志是否异步写入（true 异步，false 同步）
    bool IsAsyncLog() const;

    // listenfd 是否边缘触发（true ET，false LT）
    bool IsListenfdET() const;

    // connfd 是否边缘触发（true ET，false LT）
    bool IsConnfdET() const;

    // 数据库连接池数量
    int GetSqlPoolSize() const;

    // 线程池线程数量
    int GetThreadPoolSize() const;

    // 是否关闭日志输出
    bool GetCloseLog() const;

    // 是否为 reactor 并发模型（true reactor，false proactor）
    bool IsReactorModel() const;

private:
    int port_;                // 监听端口号
    int log_write_mode_;      // 日志写入方式：0 同步，1 异步
    int listen_trigger_mode_; // listenfd 触发模式：0 LT，1 ET
    int conn_trigger_mode_;   // connfd 触发模式：0 LT，1 ET
    int sql_pool_size_;       // 数据库连接池数量
    int thread_pool_size_;    // 线程池线程数量
    int close_log_;           // 是否关闭日志：0 否，1 是
    int concurrency_model_;   // 并发模型：0 proactor，1 reactor
};
