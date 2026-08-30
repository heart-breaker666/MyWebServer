#pragma once

#include <cstdint>
#include <ctime>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

// 定时器管理类（单例，懒加载）。
// 用于清理非活跃连接：连接创建时注册定时器，有数据传输时顺延，
// 到期后由 Tick() 返回连接 fd 交给上层关闭。
// 存储结构：小根堆（std::priority_queue，堆顶为最近到期节点）+ 句柄映射
// （fd -> 当前版本号）。priority_queue 不支持删除/修改任意节点，故采用惰性删除：
// 注册/顺延时递增版本号并将新节点压入堆，旧节点留在堆中，弹出时与句柄映射比对
// 版本号，失效节点直接丢弃。增删改均为 O(log n)，取最近到期时间为 O(1)。
// 触发方式：事件循环以最近到期时间作为 epoll_wait 超时，到期后调用 Tick()，
// 无需 alarm + SIGALRM 信号管道。
// 线程安全：互斥锁保护堆与句柄映射（主线程 Adjust/Tick 与工作线程 Close 删除并发访问）。
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

    // 定时器节点：同一 fd 在堆中可能同时存在多个版本，弹出时校验版本号丢弃旧节点
    struct TimerNode {
        int fd;           // 连接描述符
        time_t expire;    // 绝对到期时间
        uint64_t version; // 版本号（每次注册/顺延递增，标记节点新旧）
    };

    // 小根堆比较器：到期时间早的优先，同秒到期时版本小的优先（先注册的先触发）
    struct NodeCompare {
        bool operator()(const TimerNode& lhs, const TimerNode& rhs) const {
            return lhs.expire > rhs.expire ||
                   (lhs.expire == rhs.expire && lhs.version > rhs.version);
        }
    };

    // 弹出并丢弃堆顶所有失效节点（fd 已删除或版本号过期），调用方保证 mutex_ 已持有
    void CleanExpired() const;

    mutable std::priority_queue<TimerNode, std::vector<TimerNode>, NodeCompare> heap_;
                                                        // 小根堆（堆顶为最近到期节点）
    std::unordered_map<int, uint64_t> fd_version_;      // 句柄映射：fd -> 当前有效版本号
    mutable std::mutex mutex_;                          // 保护堆与句柄映射
};
