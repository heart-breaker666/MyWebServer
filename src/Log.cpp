#include "../include/Log.h"

#include <sys/time.h>

#include <cstdarg>
#include <cstring>
#include <ctime>

namespace {

// 将日志级别枚举转换为 TinyWebServer 风格的级别前缀字符串
const char* LevelToString(Log::LogLevel level) {
    switch (level) {
        case Log::LogLevel::kDebug:
            return "[debug]:";
        case Log::LogLevel::kInfo:
            return "[info]:";
        case Log::LogLevel::kWarn:
            return "[warn]:";
        case Log::LogLevel::kError:
            return "[erro]:";
        default:
            return "[info]:";
    }
}

// 生成日期前缀字符串，格式：YYYY_MM_DD_
std::string BuildDatePrefix(const std::tm& tm_now) {
    char date[32];
    std::snprintf(date, sizeof(date), "%d_%02d_%02d_",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                  tm_now.tm_mday);
    return date;
}

}  // namespace

Log& Log::GetInstance() {
    // C++11 magic static：首次调用时构造，标准保证其线程安全
    static Log instance;
    return instance;
}

Log::Log()
    : close_log_(0),
      log_buf_size_(0),
      split_lines_(0),
      max_queue_size_(0),
      log_count_(0),
      today_(0),
      file_(nullptr),
      buf_(nullptr),
      async_enabled_(false),
      stop_flag_(false),
      idle_(true) {}

Log::~Log() {
    Reset();
}

bool Log::Init(const char* file_name, int close_log, int log_buf_size,
               int split_lines, int max_queue_size) {
    // 若已初始化过，先释放旧资源
    Reset();

    close_log_ = close_log;
    log_buf_size_ = log_buf_size;
    split_lines_ = split_lines;
    max_queue_size_ = max_queue_size;
    buf_ = new char[log_buf_size_];

    // 解析目录与文件名
    const char* slash = std::strrchr(file_name, '/');
    if (slash == nullptr) {
        dir_name_.clear();
        log_name_ = file_name;
    } else {
        dir_name_.assign(file_name, slash - file_name + 1);
        log_name_ = slash + 1;
    }

    // 按天生成完整文件名：目录/YYYY_MM_DD_文件名
    std::time_t t = std::time(nullptr);
    std::tm tm_now;
    localtime_r(&t, &tm_now);
    today_ = tm_now.tm_mday;
    log_full_name_ = dir_name_ + BuildDatePrefix(tm_now) + log_name_;

    file_ = std::fopen(log_full_name_.c_str(), "a");
    if (file_ == nullptr) {
        return false;
    }

    // max_queue_size >= 1 时启用异步写入（后台线程消费有界队列）
    if (max_queue_size_ >= 1) {
        async_enabled_ = true;
        stop_flag_ = false;
        idle_ = true;
        async_thread_ = std::thread(&Log::AsyncLoop, this);
    }
    return true;
}

void Log::WriteLog(LogLevel level, const char* format, ...) {
    // 关闭日志或尚未初始化时直接丢弃
    if (close_log_ || buf_ == nullptr) {
        return;
    }

    // 获取当前时间（localtime_r 线程安全，避免并发覆盖共享 tm）
    struct timeval now;
    gettimeofday(&now, nullptr);
    std::tm tm_now;
    localtime_r(&now.tv_sec, &tm_now);

    // 行数统计、按天/行数分割与格式化（均受 file_mutex_ 保护，
    // 防止多线程并发写入共享 buf_ 导致内容相互覆盖）
    std::string line;
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        ++log_count_;
        if (today_ != tm_now.tm_mday || log_count_ % split_lines_ == 0) {
            if (file_ != nullptr) {
                std::fflush(file_);
                std::fclose(file_);
            }
            const std::string date_prefix = BuildDatePrefix(tm_now);
            std::string new_name;
            if (today_ != tm_now.tm_mday) {
                // 跨天：切换到新日期文件
                new_name = dir_name_ + date_prefix + log_name_;
                today_ = tm_now.tm_mday;
                log_count_ = 0;
            } else {
                // 行数超限：切换到带序号的新文件继续写
                new_name = dir_name_ + date_prefix + log_name_ + "." +
                           std::to_string(log_count_ / split_lines_);
            }
            file_ = std::fopen(new_name.c_str(), "a");
        }

        // 格式化日志行：时间戳 + 级别 + 内容
        va_list args;
        va_start(args, format);
        const int n = std::snprintf(buf_, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                                    tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                                    tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min,
                                    tm_now.tm_sec, now.tv_usec,
                                    LevelToString(level));
        int m = std::vsnprintf(buf_ + n, log_buf_size_ - n - 1, format, args);
        va_end(args);
        // 防止超长内容越界
        if (m < 0) {
            m = 0;
        }
        if (n + m >= log_buf_size_ - 1) {
            m = log_buf_size_ - n - 2;
        }
        buf_[n + m] = '\n';
        buf_[n + m + 1] = '\0';
        line.assign(buf_);
    }

    // 异步且队列未满：入队交给后台线程；否则同步写入文件
    if (async_enabled_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (async_queue_.size() <
            static_cast<std::size_t>(max_queue_size_)) {
            async_queue_.push_back(std::move(line));
            queue_cv_.notify_one();
            return;
        }
        // 队列满：降级为同步写入，保证日志不丢失
        lock.unlock();
    }
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        if (file_ != nullptr) {
            std::fputs(line.c_str(), file_);
            // 同步写入后立即落盘，保证日志实时可见（对齐 TinyWebServer 宏 flush 行为）
            std::fflush(file_);
        }
    }
}

void Log::Flush() {
    // 异步模式：等待后台线程消费完队列中所有日志
    if (async_enabled_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        idle_cv_.wait(lock, [this] {
            return async_queue_.empty() && idle_;
        });
    }
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (file_ != nullptr) {
        std::fflush(file_);
    }
}

void Log::AsyncLoop() {
    while (true) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stop_flag_ || !async_queue_.empty();
            });
            if (stop_flag_ && async_queue_.empty()) {
                break;
            }
            line = std::move(async_queue_.front());
            async_queue_.pop_front();
            idle_ = false;
        }
        // 在锁外执行实际写入，避免长时间占用队列锁
        {
            std::lock_guard<std::mutex> lock(file_mutex_);
            if (file_ != nullptr) {
                std::fputs(line.c_str(), file_);
                // 后台线程消费后立即落盘，保证日志实时可见
                std::fflush(file_);
            }
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (async_queue_.empty()) {
                idle_ = true;
                idle_cv_.notify_all();
            }
        }
    }
}

void Log::Reset() {
    // 停止并回收后台线程（退出前会消费完队列中剩余日志）
    if (async_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_flag_ = true;
        }
        queue_cv_.notify_all();
        async_thread_.join();
        stop_flag_ = false;
    }
    // 关闭日志文件并释放缓冲
    if (file_ != nullptr) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
    delete[] buf_;
    buf_ = nullptr;
    log_count_ = 0;
    today_ = 0;
    async_enabled_ = false;
}
