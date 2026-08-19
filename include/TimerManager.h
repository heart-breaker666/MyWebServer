#pragma once

#include <cstdint>
#include <ctime>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

// 定时器管理类（单例，懒加载）。
// 用于清理非活跃连接：连接创建时注册定时器，有数据传输时顺延，
// 到期后由 Tick() 返回连接 fd 交给上层关闭（对齐 TinyWebServer 的升序链表定时器，
// 本实现使用 std::set 红黑树，增删查改均为 O(log n)）。
// 触发方式：事件循环以最近到期时间作为 epoll_wait 超时，到期后调用 Tick()，
// 无需 alarm + SIGALRM 信号管道（比 TinyWebServer 的统一事件源方案更简洁）。
// 线程安全：互斥锁保护定时器集合（主线程 Adjust/Tick 与工作线程 Close 删除并发访问）。
class TimerManager {
public:
    static TimerManager& GetInstance() {
        static TimerManager instance;
        return instance;
    }

    // 注册定时器：fd 在 timeout_sec 秒后到期（重复注册会覆盖旧定时器）
    // fd: 连接描述符；timeout_sec: 超时秒数
    void AddTimer(int fd, int timeout_sec);

    // 顺延定时器：连接有活动时重置到期时间（无此定时器则忽略）
    void AdjustTimer(int fd, int timeout_sec);

    // 删除定时器：连接关闭时调用，防止 fd 重用后旧定时器误关新连接
    void DeleteTimer(int fd);

    // 最近一次到期时间（无定时器返回 -1），用于设置 epoll_wait 超时
    time_t GetNextExpire() const;

    // 取出并返回所有已到期连接的 fd（同时从定时器集合移除），调用方负责关闭连接
    std::vector<int> Tick();

private:
    TimerManager() = default;
    ~TimerManager() = default;
    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    // 定时器节点：按 (到期时间, 序号) 排序，序号保证同秒到期时节点唯一
    struct TimerNode {
        int fd;          // 连接描述符
        time_t expire;   // 绝对到期时间
        uint64_t seq;    // 序号（顺延/重注册时递增，保证排序稳定）
        bool operator<(const TimerNode& other) const {
            return expire < other.expire ||
                   (expire == other.expire && seq < other.seq);
        }
    };

    std::set<TimerNode> timers_;                 // 定时器集合（按到期时间升序）
    std::unordered_map<int, std::pair<time_t, uint64_t>> fd_info_;
                                                 // fd -> (到期时间, 序号)，O(1) 定位并构造删除键
    mutable std::mutex mutex_;                   // 保护定时器集合
    uint64_t next_seq_ = 0;                      // 序号生成器
};
