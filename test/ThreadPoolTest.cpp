// 线程池单元测试。
// 验证：
//   - FIFO 任务调度顺序（单线程下严格先进先出）
//   - 并发处理能力（多线程消费大量任务不丢失）
//   - HttpConn 任务通过线程池正确执行（半同步半异步 / 多线程 reactor 两种模型）
//   - 线程池状态管理（队列容量、队列满拒绝、析构回收线程）
#include "../include/ThreadPool.h"
#include "../include/HttpConn.h"
#include "../include/ConnectionPool.h"
#include "../include/Log.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
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

// 测试连接子类：覆写 HttpConn 的虚方法，Process 时记录执行顺序并递增计数，
// 用于验证线程池的任务调度逻辑（FIFO/并发/状态），不涉及真实 socket 收发
class TestConn : public HttpConn {
public:
    TestConn(std::atomic<int>& counter, std::vector<int>& order,
             std::mutex& order_mutex, int id)
        : counter_(counter),
          order_(order),
          order_mutex_(order_mutex),
          id_(id) {}

    bool Read() override {
        return true;
    }
    void Process() override {
        std::lock_guard<std::mutex> lock(order_mutex_);
        order_.push_back(id_);
        counter_.fetch_add(1);
    }
    bool Write() override {
        return true;
    }
    void Close() override {}

private:
    std::atomic<int>& counter_;    // 执行计数
    std::vector<int>& order_;      // 执行顺序记录
    std::mutex& order_mutex_;      // 保护顺序记录
    int id_;                       // 任务编号
};

// 发送阶段测试连接：统计各方法被调用的次数，
// 用于验证线程池按 HttpConn 的处理阶段（ProcessState）分发：
// 发送阶段只执行 Write，不触发 Read/Process
class WriteOnlyConn : public HttpConn {
public:
    WriteOnlyConn(int& read_calls, int& process_calls, int& write_calls)
        : read_calls_(read_calls),
          process_calls_(process_calls),
          write_calls_(write_calls) {}

    bool Read() override {
        ++read_calls_;
        return true;
    }
    void Process() override {
        ++process_calls_;
    }
    bool Write() override {
        ++write_calls_;
        return true;
    }
    void Close() override {}

private:
    int& read_calls_;      // Read 调用次数
    int& process_calls_;   // Process 调用次数
    int& write_calls_;     // Write 调用次数
};

// 等待计数达到目标
void WaitFor(const std::atomic<int>& counter, int target) {
    while (counter.load() < target) {
        std::this_thread::yield();
    }
}

// 测试 1：FIFO 调度——单线程池下执行顺序严格等于提交顺序
void TestFifo() {
    constexpr int kCount = 100;
    std::atomic<int> counter{0};
    std::vector<int> order;
    std::mutex order_mutex;
    ThreadPool pool(1, 10000);  // 单线程，严格 FIFO

    std::vector<TestConn> tasks;
    tasks.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        tasks.emplace_back(counter, order, order_mutex, i);
        CHECK(pool.Append(&tasks[i]));
    }
    WaitFor(counter, kCount);

    bool ordered = order.size() == static_cast<std::size_t>(kCount);
    for (int i = 0; ordered && i < kCount; ++i) {
        if (order[i] != i) {
            ordered = false;
        }
    }
    CHECK(ordered);
}

// 测试 2：并发处理——多线程消费 10000 个任务，全部执行无丢失
void TestConcurrency() {
    constexpr int kCount = 10000;
    std::atomic<int> counter{0};
    std::vector<int> order;
    std::mutex order_mutex;
    ThreadPool pool(8, 10000);

    std::vector<TestConn> tasks;
    tasks.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        tasks.emplace_back(counter, order, order_mutex, i);
        CHECK(pool.Append(&tasks[i]));
    }
    WaitFor(counter, kCount);

    CHECK(counter.load() == kCount);
    CHECK(order.size() == static_cast<std::size_t>(kCount));
}

// 测试 3/4：HttpConn 任务通过线程池执行（半同步半异步 / 多线程 reactor）
void TestHttpConnTask(bool reactor) {
    ThreadPool pool(2, 100, reactor ? 1 : 0);

    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    const std::string request =
        "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(fds[0], request.data(), request.size());

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    HttpConn conn;
    conn.Init(fds[1], addr, "./root", false);

    if (!reactor) {
        conn.Read();  // 半同步半异步：主线程读取数据后提交
    }
    CHECK(pool.Append(&conn));

    // 模拟主线程事件循环，等待 EPOLLOUT 事件并驱动发送。
    // 两种模式下 worker 处理完都只注册 EPOLLOUT，由事件循环驱动 Write。
    epoll_event events[8];
    bool closed = false;
    while (!closed) {
        const int num = epoll_wait(HttpConn::epoll_fd_, events, 8, 5000);
        if (num <= 0) {
            break;  // 超时：视为异常，交由下方断言暴露
        }
        for (int i = 0; i < num; ++i) {
            if (events[i].data.fd == fds[1] && (events[i].events & EPOLLOUT)) {
                if (!conn.Write() || conn.WriteDone()) {
                    conn.Close();  // 发送完成，关闭连接
                    closed = true;
                }
            }
        }
    }

    // 读取响应，直到线程池处理并关闭连接
    std::string response;
    char buf[8192];
    while (true) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        response.append(buf, n);
    }
    close(fds[0]);
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(response.find("登录") != std::string::npos);
}

// 测试 5：状态管理——队列容量限制、队列满拒绝、析构回收线程
void TestState() {
    std::atomic<int> counter{0};
    std::vector<int> order;
    std::mutex order_mutex;
    {
        // 0 线程：不消费任务，用于验证队列容量与状态管理
        ThreadPool pool(0, 5);

        std::vector<TestConn> tasks;
        for (int i = 0; i < 5; ++i) {
            tasks.emplace_back(counter, order, order_mutex, i);
            CHECK(pool.Append(&tasks[i]));
        }
        CHECK(pool.GetPendingCount() == 5);  // 等待队列积压 5 个

        TestConn extra(counter, order, order_mutex, 100);
        CHECK(!pool.Append(&extra));  // 队列满，任务被拒绝
    }  // 作用域结束：析构回收全部线程（join），此处验证不崩溃
    std::printf("ThreadPool: state management ok\n");
}

// 测试 6：处理阶段分发——发送阶段任务只执行 Write，不触发 Read/Process
// 用于验证多线程 reactor 模式 EPOLLOUT 续写时线程池按连接状态分发正确
void TestWriteStateDispatch() {
    int read_calls = 0;
    int process_calls = 0;
    int write_calls = 0;
    WriteOnlyConn conn(read_calls, process_calls, write_calls);
    ThreadPool pool(1, 10, 1);  // 多线程 reactor 单线程

    conn.SetWriteState();  // 进入发送阶段（append 前置写入状态）
    CHECK(pool.Append(&conn));
    // 等待工作线程执行完写任务
    while (write_calls == 0) {
        std::this_thread::yield();
    }
    CHECK(write_calls == 1);
    CHECK(read_calls == 0);      // 发送阶段不触发读
    CHECK(process_calls == 0);   // 发送阶段不触发处理
}

// 测试 7：半包请求——先发送请求行，worker 解析不完整时重新注册 EPOLLIN 等待补齐，
// 补齐后完整处理并返回响应（验证半同步半异步 / 多线程 reactor 两种模型的半包续读链路）
void TestHalfPacket(bool reactor) {
    ThreadPool pool(2, 100, reactor ? 1 : 0);

    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    const std::string request =
        "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::size_t first = request.find("\r\n") + 2;  // 仅请求行

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    HttpConn conn;
    conn.Init(fds[1], addr, "./root", false);  // LT：Read 只读一次

    // 第一步：只发送请求行（半包）
    write(fds[0], request.data(), first);
    if (!reactor) {
        conn.Read();  // 半同步半异步：主线程读（LT 只读一次，仅请求行）
        CHECK(pool.Append(&conn));
    } else {
        CHECK(pool.Append(&conn));  // 多线程 reactor：worker 内 Read + Process
    }

    // 等待 worker 处理完半包并重新注册 EPOLLIN
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 第二步：发送剩余请求头，模拟主线程事件循环驱动续读
    write(fds[0], request.data() + first, request.size() - first);
    epoll_event events[8];
    while (true) {
        const int num = epoll_wait(HttpConn::epoll_fd_, events, 8, 500);
        if (num <= 0) {
            break;  // 超时：多线程 reactor 下 worker 已完成处理并关闭连接
        }
        bool closed = false;
        for (int i = 0; i < num; ++i) {
            if (events[i].data.fd != fds[1]) {
                continue;
            }
            if (events[i].events & EPOLLIN) {
                // 剩余数据可读：半同步半异步主线程读后提交，多线程 reactor 直接交回线程池
                if ((!reactor && conn.Read() && pool.Append(&conn)) ||
                    (reactor && pool.Append(&conn))) {
                    continue;
                }
                conn.Close();
            } else if (events[i].events & EPOLLOUT) {
                // 发送缓冲区可写：主线程驱动发送（两种模式下 worker 处理完
                // 都只注册 EPOLLOUT，由事件循环调用 Write）
                if (!conn.Write() || conn.WriteDone()) {
                    conn.Close();
                    closed = true;
                }
            }
        }
        if (closed) {
            break;
        }
    }

    // 读取响应，直到连接关闭
    std::string response;
    char buf[8192];
    while (true) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        response.append(buf, n);
    }
    close(fds[0]);
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(response.find("登录") != std::string::npos);
}

}  // namespace

int main() {
    // 初始化日志与连接池（HttpConn 任务测试依赖）
    Log::GetInstance().Init("test_threadpool.log", 0, 8192, 5000000, 0);
    ConnectionPool::GetInstance().Init("127.0.0.1", "root", "fxh668",
                                       "yourdb", 3306, 4, 0);
    // HttpConn 静态成员：创建 epoll 供连接注册
    HttpConn::epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    HttpConn::user_count_ = 0;

    TestFifo();
    TestConcurrency();
    TestHttpConnTask(false);  // 半同步半异步
    TestHttpConnTask(true);   // 多线程 reactor
    TestState();
    TestWriteStateDispatch();  // 处理阶段分发
    TestHalfPacket(false);     // 半包续读（半同步半异步）
    TestHalfPacket(true);      // 半包续读（多线程 reactor）

    close(HttpConn::epoll_fd_);

    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
