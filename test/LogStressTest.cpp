// 日志模块高并发压力测试。
// 场景：1000 个线程并发写日志，验证：
//   - 线程安全：所有日志不丢失（文件行数 == 写入总数）
//   - 吞吐量：统计同步/异步模式下的每秒写入行数
#include "../include/Log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

// 并发线程数（模拟 1000 个并发连接各自写日志）
constexpr int kThreads = 1000;
// 每个线程写入的日志条数
constexpr int kPerThread = 100;
// 日志总条数
constexpr int kTotal = kThreads * kPerThread;

// 简易断言框架，统计失败数
int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__);          \
        }                                                                  \
    } while (0)

// 生成当天日期前缀，与 Log 内部命名一致：YYYY_MM_DD_
std::string TodayPrefix() {
    const std::time_t t = std::time(nullptr);
    std::tm tm_now;
    localtime_r(&t, &tm_now);
    char date[32];
    std::snprintf(date, sizeof(date), "%d_%02d_%02d_",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                  tm_now.tm_mday);
    return date;
}

// 统计文件中的行数（日志行数）
int CountLines(const std::string& path) {
    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    return static_cast<int>(std::count(content.begin(), content.end(), '\n'));
}

// 运行一次压力测试。
// async: true 异步写入（队列容量 1024），false 同步写入
// name:  日志文件名（不含日期前缀）
// 返回写入耗时（毫秒）
long long RunStress(bool async, const std::string& name) {
    const std::string path = TodayPrefix() + name;
    std::remove(path.c_str());

    Log& log = Log::GetInstance();
    CHECK(log.Init(name.c_str(), 0, 8192, 5000000, async ? 1024 : 0));

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("stress thread %d msg %d", t, i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    log.Flush();

    const auto end = std::chrono::steady_clock::now();
    const long long cost_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();

    const int lines = CountLines(path);
    CHECK(lines == kTotal);
    std::printf("[%s] threads=%d total=%d written=%d cost=%lldms "
                "throughput=%.0f lines/s\n",
                async ? "async" : "sync", kThreads, kTotal, lines, cost_ms,
                cost_ms > 0 ? kTotal * 1000.0 / cost_ms : 0.0);
    return cost_ms;
}

}  // namespace

int main() {
    RunStress(false, "test_stress_sync.log");
    RunStress(true, "test_stress_async.log");

    if (g_failures == 0) {
        std::printf("All stress tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
