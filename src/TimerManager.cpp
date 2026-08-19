#include "../include/TimerManager.h"

#include <ctime>

void TimerManager::AddTimer(int fd, int timeout_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 重复注册：先移除旧节点（幂等）
    const auto it = fd_info_.find(fd);
    if (it != fd_info_.end()) {
        timers_.erase(TimerNode{fd, it->second.first, it->second.second});
    }
    const uint64_t seq = next_seq_++;
    const time_t expire = std::time(nullptr) + timeout_sec;
    timers_.insert(TimerNode{fd, expire, seq});
    fd_info_[fd] = std::make_pair(expire, seq);
}

void TimerManager::AdjustTimer(int fd, int timeout_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fd_info_.find(fd);
    if (it == fd_info_.end()) {
        return;  // 无此定时器（连接已关闭）
    }
    // 移除旧节点，以新的到期时间重新插入（顺延）
    timers_.erase(TimerNode{fd, it->second.first, it->second.second});
    const uint64_t seq = next_seq_++;
    const time_t expire = std::time(nullptr) + timeout_sec;
    timers_.insert(TimerNode{fd, expire, seq});
    it->second = std::make_pair(expire, seq);
}

void TimerManager::DeleteTimer(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fd_info_.find(fd);
    if (it == fd_info_.end()) {
        return;
    }
    timers_.erase(TimerNode{fd, it->second.first, it->second.second});
    fd_info_.erase(it);
}

time_t TimerManager::GetNextExpire() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timers_.empty()) {
        return -1;
    }
    return timers_.begin()->expire;
}

std::vector<int> TimerManager::Tick() {
    std::vector<int> expired;
    const time_t now = std::time(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    // 集合按到期时间升序，从头部连续取出已到期节点
    while (!timers_.empty() && timers_.begin()->expire <= now) {
        const TimerNode node = *timers_.begin();
        timers_.erase(timers_.begin());
        fd_info_.erase(node.fd);
        expired.push_back(node.fd);
    }
    return expired;
}
