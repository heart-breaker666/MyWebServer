// 日志系统单元测试。
// 验证：单例一致性、同步/异步写入、按天命名、按行数分割、关闭日志、线程安全。
#include "../include/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
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
int CountLines(const std::string& path) {
    const std::string content = ReadFile(path);
    return static_cast<int>(std::count(content.begin(), content.end(), '\n'));
}

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

// 删除日志及其分割文件（.0、.1、.2），避免历史数据干扰
void CleanLogFiles(const std::string& name) {
    const std::string base = TodayPrefix() + name;
    std::remove(base.c_str());
    std::remove((base + ".0").c_str());
    std::remove((base + ".1").c_str());
    std::remove((base + ".2").c_str());
    std::remove((base + ".3").c_str());
}

// 测试 1：单例模式——多次获取返回同一实例，多线程并发获取也一致
void TestSingleton() {
    Log& first = Log::GetInstance();
    Log& second = Log::GetInstance();
    CHECK(&first == &second);

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([] {
            (void)Log::GetInstance();
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(&Log::GetInstance() == &first);
}

// 测试 2：同步写入——文件名为 日期前缀_文件名，内容含时间戳/级别/消息
void TestSyncWrite() {
    const std::string name = "test_sync.log";
    CleanLogFiles(name);
    Log& log = Log::GetInstance();
    CHECK(log.Init(name.c_str(), 0, 8192, 5000000, 0));

    LOG_INFO("sync message %d", 42);
    LOG_ERROR("error happened: %s", "bad_thing");
    log.Flush();

    const std::string path = TodayPrefix() + name;
    const std::string content = ReadFile(path);
    CHECK(content.find("sync message 42") != std::string::npos);
    CHECK(content.find("error happened: bad_thing") != std::string::npos);
    CHECK(content.find("[info]:") != std::string::npos);
    CHECK(content.find("[erro]:") != std::string::npos);
    CHECK(CountLines(path) == 2);
}

// 测试 3：按行数分割——超过 split_lines 行后切换到带序号的新文件
void TestSplitLines() {
    const std::string name = "test_split.log";
    CleanLogFiles(name);
    Log& log = Log::GetInstance();
    // 每 10 行分割一次
    CHECK(log.Init(name.c_str(), 0, 8192, 10, 0));

    for (int i = 0; i < 25; ++i) {
        LOG_INFO("split %d", i);
    }
    log.Flush();

    const std::string main = TodayPrefix() + name;
    const std::string part1 = main + ".1";
    const std::string part2 = main + ".2";
    // 第 1-9 行写入主文件；第 10 行起切换 .1；第 20 行起切换 .2
    CHECK(CountLines(main) == 9);
    CHECK(CountLines(part1) == 10);
    CHECK(CountLines(part2) == 6);
    const std::string main_content = ReadFile(main);
    const std::string part1_content = ReadFile(part1);
    const std::string part2_content = ReadFile(part2);
    CHECK(main_content.find("split 0") != std::string::npos);
    CHECK(main_content.find("split 9") == std::string::npos);
    CHECK(part1_content.find("split 9") != std::string::npos);
    CHECK(part1_content.find("split 19") == std::string::npos);
    CHECK(part2_content.find("split 19") != std::string::npos);
    CHECK(part2_content.find("split 24") != std::string::npos);
}

// 测试 4：异步写入——多线程并发写，队列满降级同步写也不丢日志
void TestAsyncWrite() {
    const std::string name = "test_async.log";
    CleanLogFiles(name);
    Log& log = Log::GetInstance();
    // 队列容量 1024，写入 4000 条必然触发队列满降级
    CHECK(log.Init(name.c_str(), 0, 8192, 5000000, 1024));

    const int kThreads = 8;
    const int kPerThread = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO("thread %d message %d", t, i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    log.Flush();
    CHECK(CountLines(TodayPrefix() + name) == kThreads * kPerThread);
}

// 测试 5：关闭日志——close_log=1 时日志被丢弃，文件保持为空
void TestCloseLog() {
    const std::string name = "test_close.log";
    CleanLogFiles(name);
    Log& log = Log::GetInstance();
    CHECK(log.Init(name.c_str(), 1, 8192, 5000000, 0));

    LOG_INFO("should be dropped");
    LOG_ERROR("should be dropped too");
    log.Flush();

    CHECK(CountLines(TodayPrefix() + name) == 0);
}

}  // namespace

int main() {
    TestSingleton();
    TestSyncWrite();
    TestSplitLines();
    TestAsyncWrite();
    TestCloseLog();

    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
