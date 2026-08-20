#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "../include/HttpConn.h"

// 线程池类（对齐 TinyWebServer 的 threadpool，任务类型固定为 HttpConn）。
// 基于 std::queue 的 FIFO 等待队列，固定数量工作线程消费任务；
// 任务按先进先出顺序被处理，无需模板化（服务器中任务始终是 HttpConn 连接）。
// 任务执行按 HttpConn 的处理阶段（m_state）分发（对齐 TinyWebServer run()）：
//   reactor：发送阶段仅执行 Write；读与处理阶段执行 Read+Process，
//            处理完成后注册 EPOLLOUT 交还事件循环，由 EPOLLOUT 事件把连接
//            再次送回线程池进入发送阶段（读/处理与写分离）
//   proactor：仅执行 Process（主线程负责读写），发送由主线程事件循环驱动
// 同步机制：互斥锁 + 条件变量保护任务队列，避免竞态。
class ThreadPool {
public:
    // 构造函数：创建并启动 thread_number 个工作线程
    // thread_number: 工作线程数
    // max_requests:  等待队列最大长度，超过则拒绝提交任务
    // actor_model:   并发模型，0 proactor / 1 reactor
    explicit ThreadPool(int thread_number = 8, int max_requests = 10000,
                        int actor_model = 0)
        : thread_number_(thread_number),
          max_requests_(max_requests),
          actor_model_(actor_model),
          stop_(false) {
        // 创建并启动工作线程
        for (int i = 0; i < thread_number_; ++i) {
            threads_.emplace_back(&ThreadPool::Run, this);
        }
    }

    // 析构函数：通知线程停止并回收所有工作线程
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        queue_cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    // 添加任务到等待队列（FIFO）。队列满时返回 false（任务被拒绝）。
    // request: HttpConn 连接对象指针（proactor 模式数据已由调用方读取）
    bool Append(HttpConn* request) {
        if (request == nullptr) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (work_queue_.size() >= static_cast<std::size_t>(max_requests_)) {
                return false;  // 等待队列已满
            }
            work_queue_.push(request);
        }
        queue_cv_.notify_one();
        return true;
    }

    // 当前等待队列中的任务数
    std::size_t GetPendingCount() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return work_queue_.size();
    }

    // 工作线程数
    int GetThreadCount() const {
        return thread_number_;
    }

private:
    // 工作线程主循环：从队列取出任务并执行（FIFO）
    void Run() {
        while (true) {
            HttpConn* request = nullptr;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                // 队列为空时等待，直到有任务或线程池停止
                queue_cv_.wait(lock, [this] {
                    return stop_ || !work_queue_.empty();
                });
                if (stop_ && work_queue_.empty()) {
                    return;  // 停止且队列已空，退出线程
                }
                request = work_queue_.front();
                work_queue_.pop();
            }
            if (request == nullptr) {
                continue;
            }
            if (actor_model_ == 1) {
                // reactor：按连接处理阶段分发（对齐 TinyWebServer run() 的 m_state 分支）
                if (request->IsWriteState()) {
                    // 发送阶段：仅继续发送未完成的响应。
                    // 发送失败：关闭连接；全部发完：keep-alive 则重置连接等待下一请求，
                    // 否则关闭；未发完：Write() 内部已重新注册 EPOLLOUT，
                    // 连接交由事件循环再次送回线程池，工作线程立即释放。
                    if (!request->Write()) {
                        request->Close();
                    } else if (request->WriteDone()) {
                        if (request->IsKeepAlive()) {
                            request->Reuse();
                        } else {
                            request->Close();
                        }
                    }
                    continue;
                }
                // 读与处理阶段：读取数据并构造响应，处理完成后不直接发送，
                // 而是注册 EPOLLOUT 交还事件循环，由 EPOLLOUT 事件把连接再次送回
                // 线程池进入发送阶段（读/处理与写分离，对齐 TinyWebServer）
                if (!request->Read()) {
                    request->Close();
                    continue;
                }
                request->Process();  // 处理成功后进入发送阶段
                if (request->IsWriteState()) {
                    request->EnableWrite();
                } else if (request->IsConnected()) {
                    // 请求不完整（半包）：重新注册 EPOLLIN，等待客户端继续发送
                    request->ContinueRead();
                }
                // 否则 Process 内部失败已关闭连接
            } else {
                // proactor：主线程已读好数据，线程池只负责处理；
                // 发送由主线程事件循环驱动，写操作不在工作线程执行
                request->Process();
                if (request->IsWriteState()) {
                    request->EnableWrite();  // 响应已构造，交由主线程发送
                } else if (request->IsConnected()) {
                    // 请求不完整（半包）：重新注册 EPOLLIN，等待客户端继续发送
                    request->ContinueRead();
                }
                // 否则 Process 内部失败已关闭连接
            }
        }
    }

    int thread_number_;                    // 工作线程数
    int max_requests_;                     // 等待队列最大长度
    int actor_model_;                      // 并发模型：0 proactor / 1 reactor
    std::vector<std::thread> threads_;     // 工作线程集合
    std::queue<HttpConn*> work_queue_;     // 任务队列（FIFO）
    mutable std::mutex queue_mutex_;       // 保护任务队列
    std::condition_variable queue_cv_;     // 任务就绪通知
    bool stop_;                            // 线程池停止标志
};
