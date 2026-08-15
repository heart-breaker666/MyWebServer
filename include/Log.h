#pragma once

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class Log {
public:
    // 日志级别
    enum LogLevel {
        kDebug = 0,  // 调试
        kInfo,       // 信息
        kWarn,       // 警告
        kError       // 错误
    };

    // 获取全局唯一实例（懒汉式，线程安全）
    static Log& GetInstance();

    // 初始化日志系统。
    bool Init(const char* file_name, int close_log, int log_buf_size = 8192,
              int split_lines = 5000000, int max_queue_size = 0);
    // 写入一条日志。
    void WriteLog(LogLevel level, const char* format, ...)
        __attribute__((format(printf, 3, 4)));

    // 将缓冲区内容落盘（异步模式等待队列全部消费完毕）
    void Flush();

private:
    Log();
    ~Log();

    // 禁用拷贝构造与赋值
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    // 释放旧资源（停止后台线程、关闭文件、释放缓冲），供重复 Init 与析构使用
    void Reset();

    // 异步后台线程主循环：从队列取出日志并写入文件
    void AsyncLoop();

    bool close_log_;          // 是否关闭日志：1 关闭，0 开启
    int log_buf_size_;        // 单条日志格式化缓冲区大小
    int split_lines_;         // 单个日志文件最大行数
    int max_queue_size_;      // 异步队列容量（0 表示同步写入）
    long long log_count_;     // 当前日志文件已写入行数
    int today_;               // 当前日志文件对应的日期（月中的日，跨天判断用）
    std::FILE* file_;         // 当前日志文件句柄
    std::string dir_name_;    // 日志文件所在目录（含末尾 '/'，无路径时为空）
    std::string log_name_;    // 日志文件名（不含日期前缀）
    std::string log_full_name_;  // 当前完整文件名
    char* buf_;               // 日志格式化缓冲区

    bool async_enabled_;               // 是否异步写入
    std::deque<std::string> async_queue_;  // 异步日志队列（有界）
    std::mutex queue_mutex_;           // 保护异步队列
    std::condition_variable queue_cv_;  // 通知后台线程有新日志
    std::thread async_thread_;         // 异步后台线程
    bool stop_flag_;                   // 停止后台线程的标志
    bool idle_;                        // 队列空且最后一条已落盘
    std::condition_variable idle_cv_;  // 通知 Flush 等待队列清空

    std::mutex file_mutex_;            // 保护文件写入与分割
};

// 日志宏封装（TinyWebServer 风格，不带模块参数）
#define LOG_DEBUG(...) \
    Log::GetInstance().WriteLog(Log::LogLevel::kDebug, __VA_ARGS__)
#define LOG_INFO(...) \
    Log::GetInstance().WriteLog(Log::LogLevel::kInfo, __VA_ARGS__)
#define LOG_WARN(...) \
    Log::GetInstance().WriteLog(Log::LogLevel::kWarn, __VA_ARGS__)
#define LOG_ERROR(...) \
    Log::GetInstance().WriteLog(Log::LogLevel::kError, __VA_ARGS__)
