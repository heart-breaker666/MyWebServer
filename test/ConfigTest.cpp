// 配置模块单元测试（轻量版）。
// 验证：默认值、getopt 命令行参数解析、触发组合模式。
#include "../include/Config.h"

#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__);          \
        }                                                                  \
    } while (0)

// 构造命令行参数并解析
void Parse(Config& config, std::initializer_list<std::string> args) {
    std::vector<std::string> storage(args);
    std::vector<char*> argv;
    for (auto& s : storage) {
        argv.push_back(&s[0]);
    }
    optind = 1;  // 重置 getopt 全局状态
    config.ParseArg(static_cast<int>(argv.size()), argv.data());
}

// 测试 1：默认值——未传任何参数时使用内置默认值
void TestDefaults() {
    Config config;
    CHECK(config.GetPort() == 9006);
    CHECK(config.IsAsyncLog() == false);
    CHECK(config.IsListenfdET() == false);
    CHECK(config.IsConnfdET() == false);
    CHECK(config.GetSqlPoolSize() == 8);
    CHECK(config.GetThreadPoolSize() == 8);
    CHECK(config.GetCloseLog() == false);
    CHECK(config.IsReactorModel() == false);
}

// 测试 2：端口与整数参数解析
void TestIntArgs() {
    Config config;
    Parse(config, {"server", "-p", "8080", "-s", "16", "-t", "32"});
    CHECK(config.GetPort() == 8080);
    CHECK(config.GetSqlPoolSize() == 16);
    CHECK(config.GetThreadPoolSize() == 32);
}

// 测试 3：开关类参数（日志方式/关闭日志/并发模型）
void TestFlagArgs() {
    Config config;
    Parse(config, {"server", "-l", "1", "-c", "1", "-a", "1"});
    CHECK(config.IsAsyncLog() == true);
    CHECK(config.GetCloseLog() == true);
    CHECK(config.IsReactorModel() == true);
}

// 测试 4：触发组合模式 0 = listenfd LT + connfd LT
void TestTriggerMode0() {
    Config config;
    Parse(config, {"server", "-m", "0"});
    CHECK(config.IsListenfdET() == false);
    CHECK(config.IsConnfdET() == false);
}

// 测试 5：触发组合模式 1 = listenfd LT + connfd ET
void TestTriggerMode1() {
    Config config;
    Parse(config, {"server", "-m", "1"});
    CHECK(config.IsListenfdET() == false);
    CHECK(config.IsConnfdET() == true);
}

// 测试 6：触发组合模式 2 = listenfd ET + connfd LT
void TestTriggerMode2() {
    Config config;
    Parse(config, {"server", "-m", "2"});
    CHECK(config.IsListenfdET() == true);
    CHECK(config.IsConnfdET() == false);
}

// 测试 7：触发组合模式 3 = listenfd ET + connfd ET
void TestTriggerMode3() {
    Config config;
    Parse(config, {"server", "-m", "3"});
    CHECK(config.IsListenfdET() == true);
    CHECK(config.IsConnfdET() == true);
}

// 测试 8：全部参数组合解析
void TestCombined() {
    Config config;
    Parse(config, {"server", "-p", "7000", "-l", "1", "-m", "2",
                   "-s", "12", "-t", "24", "-c", "1", "-a", "1"});
    CHECK(config.GetPort() == 7000);
    CHECK(config.IsAsyncLog() == true);
    CHECK(config.IsListenfdET() == true);
    CHECK(config.IsConnfdET() == false);
    CHECK(config.GetSqlPoolSize() == 12);
    CHECK(config.GetThreadPoolSize() == 24);
    CHECK(config.GetCloseLog() == true);
    CHECK(config.IsReactorModel() == true);
}

}  // namespace

int main() {
    TestDefaults();
    TestIntArgs();
    TestFlagArgs();
    TestTriggerMode0();
    TestTriggerMode1();
    TestTriggerMode2();
    TestTriggerMode3();
    TestCombined();

    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
