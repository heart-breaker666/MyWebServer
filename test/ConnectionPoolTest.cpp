// 数据库连接池单元测试。
// 验证：初始化建连、获取/释放连接、队列空时阻塞等待、RAII 自动归还、销毁连接池。
// 依赖本地 MySQL（凭据与 yourdb 库），连接失败时测试不通过。
#include "../include/ConnectionPool.h"
#include "../include/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

// MySQL 测试凭据（与本地环境一致）
const char* kUrl = "127.0.0.1";
const char* kUser = "root";
const char* kPassword = "fxh668";
const char* kDatabase = "yourdb";
const int kPort = 3306;
const int kMaxConn = 4;

// 简易断言框架，统计失败数
int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__);          \
        }                                                                  \
    } while (0)

// 测试 1：初始化后池内有 max_conn 条空闲连接，获取连接成功
void TestInitAndGet() {
    ConnectionPool& pool = ConnectionPool::GetInstance();
    pool.Init(kUrl, kUser, kPassword, kDatabase, kPort, kMaxConn, 0);
    CHECK(pool.GetFreeConnCount() == kMaxConn);

    MYSQL* conn = pool.GetConnection();
    CHECK(conn != nullptr);
    CHECK(pool.GetFreeConnCount() == kMaxConn - 1);
    pool.ReleaseConnection(conn);
    CHECK(pool.GetFreeConnCount() == kMaxConn);
}

// 测试 2：取光全部连接后空闲数为 0，释放后恢复
void TestGetAllAndRelease() {
    ConnectionPool& pool = ConnectionPool::GetInstance();
    pool.Init(kUrl, kUser, kPassword, kDatabase, kPort, kMaxConn, 0);

    std::vector<MYSQL*> held;
    for (int i = 0; i < kMaxConn; ++i) {
        held.push_back(pool.GetConnection());
    }
    CHECK(pool.GetFreeConnCount() == 0);

    // 释放一条连接后空闲数恢复为 1（并从 held 中移除，避免重复释放）
    pool.ReleaseConnection(held.back());
    held.pop_back();
    CHECK(pool.GetFreeConnCount() == 1);

    for (auto* conn : held) {
        pool.ReleaseConnection(conn);
    }
    CHECK(pool.GetFreeConnCount() == kMaxConn);
}

// 测试 3：队列为空时 GetConnection 阻塞等待，连接被释放后唤醒
void TestBlockingGet() {
    ConnectionPool& pool = ConnectionPool::GetInstance();
    pool.Init(kUrl, kUser, kPassword, kDatabase, kPort, kMaxConn, 0);

    std::vector<MYSQL*> held;
    for (int i = 0; i < kMaxConn; ++i) {
        held.push_back(pool.GetConnection());
    }
    CHECK(pool.GetFreeConnCount() == 0);

    std::atomic<bool> got{false};
    std::thread waiter([&pool, &got] {
        MYSQL* conn = pool.GetConnection();  // 队列空，应阻塞等待
        pool.ReleaseConnection(conn);        // 拿完立即归还，避免泄漏
        got.store(true);
    });

    // 等待片刻，确认 waiter 仍在阻塞
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!got.load());

    // 归还一条连接，waiter 应被唤醒并拿到连接
    pool.ReleaseConnection(held.back());
    held.pop_back();
    waiter.join();
    CHECK(got.load());

    for (auto* conn : held) {
        pool.ReleaseConnection(conn);
    }
    CHECK(pool.GetFreeConnCount() == kMaxConn);
}

// 测试 4：RAII 封装——作用域结束自动归还连接
void TestRAII() {
    ConnectionPool& pool = ConnectionPool::GetInstance();
    pool.Init(kUrl, kUser, kPassword, kDatabase, kPort, kMaxConn, 0);
    CHECK(pool.GetFreeConnCount() == kMaxConn);

    {
        ConnectionRAII guard(pool);
        CHECK(guard.Get() != nullptr);
        CHECK(pool.GetFreeConnCount() == kMaxConn - 1);
    }
    CHECK(pool.GetFreeConnCount() == kMaxConn);
}

// 测试 5：销毁连接池后空闲连接数为 0
void TestDestroy() {
    ConnectionPool& pool = ConnectionPool::GetInstance();
    pool.Init(kUrl, kUser, kPassword, kDatabase, kPort, kMaxConn, 0);
    CHECK(pool.GetFreeConnCount() == kMaxConn);

    pool.DestroyPool();
    CHECK(pool.GetFreeConnCount() == 0);
}

}  // namespace

int main() {
    // 先初始化日志，供连接池初始化失败时输出错误信息
    Log::GetInstance().Init("test_pool.log", 0, 8192, 5000000, 0);

    TestInitAndGet();
    TestGetAllAndRelease();
    TestBlockingGet();
    TestRAII();
    TestDestroy();

    ConnectionPool::GetInstance().DestroyPool();

    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
