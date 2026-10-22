// 定时器管理单元测试。
// 验证：注册后到期时间正确、到期 Tick 返回并移除、顺延后原时间不触发、
//       删除后不再到期、多 fd 按到期顺序返回、多线程并发访问无崩溃。
#include "../include/TimerManager.h"

#include <atomic>
#include <cstdio>
#include <ctime>
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

// 测试 1：注册定时器后最近到期时间正确
void TestAddAndNextExpire() {
    TimerManager& tm = TimerManager::GetInstance();
    tm.AddTimer(10, 60);  // 60 秒后到期
    const time_t next = tm.GetNextExpire();
    const time_t now = std::time(nullptr);
    CHECK(next > now);          // 到期时间在未来
    CHECK(next <= now + 60);    // 不超过注册的秒数
    tm.DeleteTimer(10);
    CHECK(tm.GetNextExpire() == -1);  // 删除后无定时器
}

// 测试 2：到期后 Tick 返回对应 fd 并移除
void TestTickExpired() {
    TimerManager& tm = TimerManager::GetInstance();
    tm.AddTimer(11, 0);  // 立即到期
    tm.AddTimer(12, 0);
    const std::vector<int> expired = tm.Tick();
    CHECK(expired.size() == 2);
    const bool has11 = expired[0] == 11 || expired[1] == 11;
    const bool has12 = expired[0] == 12 || expired[1] == 12;
    CHECK(has11 && has12);
    CHECK(tm.Tick().empty());           // 已清空
    CHECK(tm.GetNextExpire() == -1);
}

// 测试 3：顺延后原到期时间不再触发
void TestAdjust() {
    TimerManager& tm = TimerManager::GetInstance();
    tm.AddTimer(13, 0);      // 立即到期
    tm.AdjustTimer(13, 60);  // 顺延 60 秒
    CHECK(tm.Tick().empty());               // 原到期时间不触发
    CHECK(tm.GetNextExpire() > std::time(nullptr));
    tm.DeleteTimer(13);
}

// 测试 4：删除后不再到期（含删除不存在定时器的幂等性）
void TestDelete() {
    TimerManager& tm = TimerManager::GetInstance();
    tm.AddTimer(14, 0);
    tm.DeleteTimer(14);
    CHECK(tm.Tick().empty());
    CHECK(tm.GetNextExpire() == -1);
    tm.DeleteTimer(999);  // 删除不存在的定时器，不崩溃
}

// 测试 5：多 fd 按到期时间顺序返回
void TestOrder() {
    TimerManager& tm = TimerManager::GetInstance();
    tm.AddTimer(20, 0);    // 先到期
    tm.AddTimer(21, 100);  // 后到期
    const std::vector<int> expired = tm.Tick();
    CHECK(expired.size() == 1);
    CHECK(expired[0] == 20);
    tm.DeleteTimer(21);
    CHECK(tm.GetNextExpire() == -1);
}

// 测试 6：多线程并发注册/顺延/删除（验证互斥锁保护，无崩溃无残留）
void TestConcurrent() {
    TimerManager& tm = TimerManager::GetInstance();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&tm, t]() {
            for (int i = 0; i < 2000; ++i) {
                const int fd = 100 + t * 2000 + i;
                tm.AddTimer(fd, 10);
                tm.AdjustTimer(fd, 10);
                tm.DeleteTimer(fd);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK(tm.GetNextExpire() == -1);  // 全部已删除，无残留
}

}  // namespace

int main() {
    TestAddAndNextExpire();
    TestTickExpired();
    TestAdjust();
    TestDelete();
    TestOrder();
    TestConcurrent();

    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
