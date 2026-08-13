#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// 日志系统单例类。
// 采用懒汉模式（C++11 magic static）保证全局唯一实例，且线程安全地延迟初始化。
// 支持同步写入与异步写入（后台线程消费队列），支持控制台/文件/两者输出，
// 支持按文件大小轮转切割。
class Log {
public:
    // 日志级别，数值越小优先级越高
    enum class LogLevel {
        kDebug = 0,
        kInfo,
        kWarn,
        kError,
        kFatal
    };

    // 日志输出目标
    enum class OutputTarget {
        kConsole,  // 仅控制台输出
        kFile,     // 仅文件输出
        kBoth      // 控制台与文件同时输出
    };

    // 获取全局唯一实例（懒汉式，线程安全）
    static Log& GetInstance();

    // 初始化日志系统。
    // target:      输出目标
    // file_path:   日志文件路径，仅当输出目标为文件/两者时生效
    // max_file_size: 单个日志文件最大字节数，超过后触发轮转切割
    // async_enabled: true 表示异步写入，false 表示同步写入
    void Init(OutputTarget target, const std::string& file_path,
              std::size_t max_file_size = 10 * 1024 * 1024,
              bool async_enabled = false);

    // 设置最低日志级别，低于该级别的日志将被丢弃
    void SetLevel(LogLevel level);

    // 获取当前日志级别
    LogLevel GetLevel() const;

    // 切换同步/异步写入模式
    // async_enabled: true 为异步，false 为同步
    void SetAsyncEnabled(bool async_enabled);

    // 查询当前是否处于异步写入模式
    bool IsAsyncEnabled() const;

    // 写入一条日志（统一入口）。
    // level:  日志级别
    // module: 产生日志的模块名称
    // fmt:    格式化字符串，与 printf 一致
    void Write(LogLevel level, const std::string& module,
               const char* fmt, ...) __attribute__((format(printf, 4, 5)));

    // 强制将缓冲区内容落盘（同步模式直接 flush 文件，
    // 异步模式等待队列全部消费完毕）
    void Flush();

    // 关闭日志系统：停止后台线程、落盘并关闭文件
    void Shutdown();

    // 将日志级别枚举转换为字符串
    static const char* LevelToString(LogLevel level);

private:
    // 私有构造函数/析构函数，禁止外部创建实例
    Log();
    ~Log();

    // 禁用拷贝构造与赋值
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    // 将格式化后的日志行写入到目标位置（同步写核心逻辑）
    void WriteLine(const std::string& line);

    // 写入单个日志文件（加锁保护）
    void WriteToFile(const std::string& line);

    // 检查并执行日志文件轮转（按大小）
    void RotateIfNeeded(std::size_t message_size);

    // 异步后台线程主循环：从队列取出日志并落盘
    void AsyncLoop();

    // 日志级别
    LogLevel level_;

    // 输出目标
    OutputTarget target_;

    // 日志文件路径
    std::string file_path_;

    // 单文件最大字节数，超过触发轮转
    std::size_t max_file_size_;

    // 当前日志文件已写入的字节数
    std::size_t current_file_size_;

    // 当前日志文件句柄，nullptr 表示文件未打开
    std::FILE* file_;

    // 是否异步写入（原子类型保证并发读写安全）
    std::atomic<bool> async_enabled_;

    // 异步日志队列
    std::deque<std::string> async_queue_;

    // 保护异步队列的互斥锁
    std::mutex queue_mutex_;

    // 通知后台线程有新日志的变量
    std::condition_variable queue_cv_;

    // 异步后台线程
    std::thread async_thread_;

    // 停止异步后台线程的标志
    bool stop_flag_;

    // 后台线程是否处于空闲状态（队列空且最后一条已写完）
    bool idle_;

    // 通知 Flush 等待队列清空的条件变量
    std::condition_variable idle_cv_;

    // 保护控制台输出（stdout）的互斥锁
    std::mutex console_mutex_;

    // 保护文件输出与轮转的互斥锁
    std::mutex file_mutex_;

    // 单条日志消息缓冲区最大字节数
    static constexpr std::size_t kMaxMessageSize = 4096;
};

// 日志级别低于当前级别时直接丢弃，不进入格式化流程
#define LOG_DEBUG(module, ...)                                          \
    do {                                                                \
        Log& log = Log::GetInstance();                                  \
        if (log.GetLevel() <= Log::LogLevel::kDebug) {                  \
            log.Write(Log::LogLevel::kDebug, module, __VA_ARGS__);      \
        }                                                               \
    } while (0)

#define LOG_INFO(module, ...)                                           \
    do {                                                                \
        Log& log = Log::GetInstance();                                  \
        if (log.GetLevel() <= Log::LogLevel::kInfo) {                   \
            log.Write(Log::LogLevel::kInfo, module, __VA_ARGS__);       \
        }                                                               \
    } while (0)

#define LOG_WARN(module, ...)                                           \
    do {                                                                \
        Log& log = Log::GetInstance();                                  \
        if (log.GetLevel() <= Log::LogLevel::kWarn) {                   \
            log.Write(Log::LogLevel::kWarn, module, __VA_ARGS__);       \
        }                                                               \
    } while (0)

#define LOG_ERROR(module, ...)                                          \
    do {                                                                \
        Log& log = Log::GetInstance();                                  \
        if (log.GetLevel() <= Log::LogLevel::kError) {                  \
            log.Write(Log::LogLevel::kError, module, __VA_ARGS__);      \
        }                                                               \
    } while (0)

#define LOG_FATAL(module, ...)                                          \
    do {                                                                \
        Log& log = Log::GetInstance();                                  \
        log.Write(Log::LogLevel::kFatal, module, __VA_ARGS__);          \
        log.Flush();                                                    \
    } while (0)
