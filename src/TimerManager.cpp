#include "../include/TimerManager.h"

#include <ctime>

void TimerManager::CleanExpired() const {
    while (!heap_.empty()) {
        const TimerNode& top = heap_.top();
        const auto it = fd_version_.find(top.fd);
        if (it == fd_version_.end() || it->second != top.version) {
            heap_.pop();  // 失效节点（已删除或旧版本），丢弃
        } else {
            break;
        }
    }
}

void TimerManager::AddTimer(int fd, int timeout_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 重复注册时版本递增，使堆中旧节点失效
    const uint64_t version = fd_version_[fd] + 1;
    fd_version_[fd] = version;
    heap_.push(TimerNode{fd, std::time(nullptr) + timeout_sec, version});
}

void TimerManager::AdjustTimer(int fd, int timeout_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fd_version_.find(fd);
    if (it == fd_version_.end()) {
        return;  // 无此定时器（连接已关闭）
    }
    const uint64_t version = it->second + 1;
    it->second = version;
    // 惰性删除：旧节点留在堆中，以新版本号压入新节点
    heap_.push(TimerNode{fd, std::time(nullptr) + timeout_sec, version});
}

void TimerManager::DeleteTimer(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 移除句柄映射即可：堆中残留节点因 fd 不存在而被 CleanExpired 丢弃
    fd_version_.erase(fd);
}

time_t TimerManager::GetNextExpire() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CleanExpired();
    if (heap_.empty()) {
        return -1;
    }
    return heap_.top().expire;
}

std::vector<int> TimerManager::Tick() {
    std::vector<int> expired;
    const time_t now = std::time(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    while (true) {
        CleanExpired();
        if (heap_.empty() || heap_.top().expire > now) {
            break;
        }
        const TimerNode node = heap_.top();
        heap_.pop();
        expired.push_back(node.fd);
    }
    return expired;
}
