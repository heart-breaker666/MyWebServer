// 日志系统单元测试。
// 验证：单例一致性、同步/异步写入、级别过滤、线程安全、文件轮转。
#include "Log.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

// 简易断言框架，统计失败数
int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__);          \
        }                                                                  \
    } while (0)

// 读取文件全部内容
std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// 统计文件中的行数（日志行数）
int CountLines(const std::string& content) {
    return static_cast<int>(std::count(content.begin(), content.end(), '\n'));
}

// 测试 1：单例模式——多次获取返回同一实例，多线程并发获取也一致
void TestSingleton() {
    Log& first = Log::GetInstance();
    Log& second = Log::GetInstance();
    CHECK(&first == &second);

    std::atomic<Log*> concurrent{nullptr};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&concurrent] {
            concurrent.store(&Log::GetInstance());
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(concurrent.load() == &first);
}

// 测试 2：同步写入——立即写入文件，包含时间戳/级别/模块/内容
void TestSyncWrite() {
    const std::string path = "test_sync.log";
    std::remove(path.c_str());
    Log& log = Log::GetInstance();
    log.Init(Log::OutputTarget::kFile, path, 0, false);
    log.SetLevel(Log::LogLevel::kDebug);

    LOG_INFO("TestModule", "sync message %d", 42);
    LOG_ERROR("TestModule", "error happened: %s", "bad_thing");

    log.Flush();
    const std::string content = ReadFile(path);
    CHECK(content.find("sync message 42") != std::string::npos);
    CHECK(content.find("error happened: bad_thing") != std::string::npos);
    // 标准化格式：时间戳与级别
    CHECK(content.find("[INFO]") != std::string::npos);
    CHECK(content.find("[ERROR]") != std::string::npos);
    CHECK(content.find("[TestModule]") != std::string::npos);
    CHECK(CountLines(content) == 2);
}

// 测试 3：级别过滤——低于设置级别的日志被丢弃
void TestLevelFilter() {
    const std::string path = "test_level.log";
    std::remove(path.c_str());
    Log& log = Log::GetInstance();
    log.Init(Log::OutputTarget::kFile, path, 0, false);
    log.SetLevel(Log::LogLevel::kWarn);

    LOG_DEBUG("TestModule", "debug message");
    LOG_INFO("TestModule", "info message");
    LOG_WARN("TestModule", "warn message");
    LOG_FATAL("TestModule", "fatal message");

    log.Flush();
    const std::string content = ReadFile(path);
    CHECK(content.find("debug message") == std::string::npos);
    CHECK(content.find("info message") == std::string::npos);
    CHECK(content.find("warn message") != std::string::npos);
    CHECK(content.find("fatal message") != std::string::npos);
    CHECK(CountLines(content) == 2);
}

// 测试 4：异步写入——后台线程消费队列，Flush 后全部落盘
void TestAsyncWrite() {
    const std::string path = "test_async.log";
    std::remove(path.c_str());
    Log& log = Log::GetInstance();
    log.Init(Log::OutputTarget::kFile, path, 0, true);
    log.SetLevel(Log::LogLevel::kDebug);
    CHECK(log.IsAsyncEnabled());

    const int kCount = 2000;
    for (int i = 0; i < kCount; ++i) {
        LOG_INFO("TestModule", "async message %d", i);
    }
    log.Flush();
    CHECK(CountLines(ReadFile(path)) == kCount);
}

// 测试 5：线程安全——多线程并发写入，日志不丢失、不崩溃
void TestThreadSafety() {
    const std::string path = "test_thread.log";
    std::remove(path.c_str());
    Log& log = Log::GetInstance();
    log.Init(Log::OutputTarget::kFile, path, 0, true);
    log.SetLevel(Log::LogLevel::kDebug);

    const int kThreads = 8;
    const int kPerThread = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("TestModule", "thread %d message %d", t, i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    log.Flush();
    CHECK(CountLines(ReadFile(path)) == kThreads * kPerThread);
}

// 测试 6：文件轮转——超过大小上限后切割出 .1 文件
void TestRotate() {
    const std::string path = "test_rotate.log";
    std::remove(path.c_str());
    std::remove((path + ".1").c_str());
    Log& log = Log::GetInstance();
    // 单文件上限 200 字节，很快触发轮转
    log.Init(Log::OutputTarget::kFile, path, 200, false);
    log.SetLevel(Log::LogLevel::kDebug);

    for (int i = 0; i < 100; ++i) {
        LOG_INFO("TestModule", "rotate message %d abcdefghijklmnopqrstuvwxyz", i);
    }
    log.Flush();

    std::FILE* rotated = std::fopen((path + ".1").c_str(), "r");
    CHECK(rotated != nullptr);
    if (rotated != nullptr) {
        std::fclose(rotated);
    }
    // 当前文件也非空
    CHECK(!ReadFile(path).empty());
}

}  // namespace

int main() {
    TestSingleton();
    TestSyncWrite();
    TestLevelFilter();
    TestAsyncWrite();
    TestThreadSafety();
    TestRotate();

    Log::GetInstance().Shutdown();

    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
