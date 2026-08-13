#include "Log.h"

#include <chrono>
#include <cstdarg>
#include <cstring>
#include <ctime>

namespace {

// 生成当前时间的格式化字符串，格式：YYYY-MM-DD HH:MM:SS.mmm
std::string GetCurrentTime() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_r(&now_time, &tm_buf);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             static_cast<int>(ms.count()));
    return buf;
}

}  // namespace

Log& Log::GetInstance() {
    // C++11 magic static：首次调用时构造，标准保证其线程安全
    static Log instance;
    return instance;
}

Log::Log()
    : level_(LogLevel::kDebug),
      target_(OutputTarget::kConsole),
      max_file_size_(0),
      current_file_size_(0),
      file_(nullptr),
      async_enabled_(false),
      stop_flag_(false),
      idle_(true) {}

Log::~Log() {
    Shutdown();
}

void Log::Init(OutputTarget target, const std::string& file_path,
               std::size_t max_file_size, bool async_enabled) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    // 若已有打开的文件，先关闭
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    target_ = target;
    file_path_ = file_path;
    max_file_size_ = max_file_size;
    current_file_size_ = 0;

    if (target_ == OutputTarget::kFile || target_ == OutputTarget::kBoth) {
        file_ = std::fopen(file_path_.c_str(), "a");
        if (file_ != nullptr) {
            // 追加模式下定位文件末尾，统计已写入的字节数用于轮转判断
            std::fseek(file_, 0, SEEK_END);
            current_file_size_ = static_cast<std::size_t>(std::ftell(file_));
        }
    }

    async_enabled_ = async_enabled;
    if (async_enabled_ && !async_thread_.joinable()) {
        stop_flag_ = false;
        idle_ = true;
        async_thread_ = std::thread(&Log::AsyncLoop, this);
    }
}

void Log::SetLevel(LogLevel level) {
    level_ = level;
}

Log::LogLevel Log::GetLevel() const {
    return level_;
}

void Log::SetAsyncEnabled(bool async_enabled) {
    if (async_enabled == async_enabled_.load()) {
        return;
    }
    if (async_enabled) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        async_enabled_ = true;
        if (!async_thread_.joinable()) {
            stop_flag_ = false;
            idle_ = true;
            async_thread_ = std::thread(&Log::AsyncLoop, this);
        }
    } else {
        // 关闭异步：通知后台线程退出并等待其消费完剩余日志
        std::unique_lock<std::mutex> lock(queue_mutex_);
        async_enabled_ = false;
        if (async_thread_.joinable()) {
            stop_flag_ = true;
            queue_cv_.notify_all();
            lock.unlock();
            async_thread_.join();
            lock.lock();
            stop_flag_ = false;
            idle_ = true;
        }
    }
}

bool Log::IsAsyncEnabled() const {
    return async_enabled_.load();
}

void Log::Write(LogLevel level, const std::string& module, const char* fmt, ...) {
    // 级别过滤：低于当前设置级别的日志直接丢弃
    if (level < level_) {
        return;
    }

    // 格式化用户消息
    va_list args;
    va_start(args, fmt);
    char message[kMaxMessageSize];
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    // 组装标准化日志行：时间戳 | 级别 | 模块 | 内容
    std::string line = "[" + GetCurrentTime() + "][" +
                       LevelToString(level) + "][" + module + "] " +
                       message + "\n";

    // 异步模式下入队由后台线程消费；FATAL 强制同步写入确保不丢失
    if (async_enabled_.load() && level != LogLevel::kFatal) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            async_queue_.push_back(std::move(line));
        }
        queue_cv_.notify_one();
    } else {
        WriteLine(line);
    }
}

void Log::WriteLine(const std::string& line) {
    if (target_ == OutputTarget::kConsole || target_ == OutputTarget::kBoth) {
        std::lock_guard<std::mutex> lock(console_mutex_);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }
    if (target_ == OutputTarget::kFile || target_ == OutputTarget::kBoth) {
        WriteToFile(line);
    }
}

void Log::WriteToFile(const std::string& line) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (file_ == nullptr) {
        return;
    }
    RotateIfNeeded(line.size());
    std::fwrite(line.data(), 1, line.size(), file_);
    current_file_size_ += line.size();
}

void Log::RotateIfNeeded(std::size_t message_size) {
    // max_file_size_ 为 0 表示不轮转
    if (max_file_size_ == 0 ||
        current_file_size_ + message_size <= max_file_size_) {
        return;
    }
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    // 查找当前已存在的最大的轮转序号
    int max_index = 0;
    while (true) {
        const std::string old_path =
            file_path_ + "." + std::to_string(max_index + 1);
        std::FILE* probe = std::fopen(old_path.c_str(), "r");
        if (probe == nullptr) {
            break;
        }
        std::fclose(probe);
        ++max_index;
    }
    // 从大到小依次后移，为新的轮转文件腾出 .1 位置
    for (int i = max_index; i >= 1; --i) {
        const std::string from = file_path_ + "." + std::to_string(i);
        const std::string to = file_path_ + "." + std::to_string(i + 1);
        std::rename(from.c_str(), to.c_str());
    }
    std::rename(file_path_.c_str(), (file_path_ + ".1").c_str());
    // 重新打开当前日志文件
    file_ = std::fopen(file_path_.c_str(), "a");
    current_file_size_ = 0;
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
        WriteLine(line);
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (async_queue_.empty()) {
                idle_ = true;
                idle_cv_.notify_all();
            }
        }
    }
}

void Log::Flush() {
    if (async_enabled_.load()) {
        // 异步模式：等待后台线程消费完队列中所有日志
        std::unique_lock<std::mutex> lock(queue_mutex_);
        idle_cv_.wait(lock, [this] {
            return async_queue_.empty() && idle_;
        });
    }
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (file_ != nullptr) {
        std::fflush(file_);
    }
    std::fflush(stdout);
}

void Log::Shutdown() {
    if (stop_flag_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_flag_ = true;
    }
    queue_cv_.notify_all();
    if (async_thread_.joinable()) {
        async_thread_.join();
    }
    Flush();
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }
}

const char* Log::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarn:
            return "WARN";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kFatal:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}
