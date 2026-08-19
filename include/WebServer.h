#pragma once

#include <sys/epoll.h>

#include <string>

#include "../include/Config.h"
#include "../include/ConnectionPool.h"
#include "../include/HttpConn.h"
#include "../include/Log.h"
#include "../include/ThreadPool.h"
#include "../include/TimerManager.h"

// Web 服务器类（对齐 TinyWebServer 的 webserver 设计）。
// 职责：封装各模块初始化（日志、数据库连接池、线程池、事件模式、监听 socket）与
//       epoll 事件循环（eventLoop）；使用 HttpConn 数组（users，fd 索引）管理连接，
//       将连接提交到线程池（ThreadPool，任务固定为 HttpConn）实现任务的自动分配与并发处理。
// 调用方式：main 解析 Config 后构造 WebServer 并调用 Run() 即可启动。
class WebServer {
public:
    // 构造函数：保存配置并初始化事件模式与连接数组
    explicit WebServer(const Config& config);

    ~WebServer();

    // 启动服务器：初始化 socket/epoll 后进入事件循环，阻塞运行
    void Run();

private:
    // 初始化日志系统（对齐 TinyWebServer log_write()）
    void InitLog();

    // 初始化数据库连接池（对齐 TinyWebServer sql_pool()）
    void InitSqlPool();

    // 初始化线程池（对齐 TinyWebServer thread_pool()）
    void InitThreadPool();

    // 根据配置设置监听/连接的触发模式（对齐 TinyWebServer trig_mode()）
    void InitEventMode();

    // 创建监听 socket、epoll 并注册监听描述符（对齐 eventListen）
    void InitSocket();

    // 将 fd 注册到 epoll（is_listen 决定使用监听/连接触发模式）
    void AddFd(int fd, bool is_listen);

    // 设置 fd 为非阻塞
    static void SetNonBlocking(int fd);

    // 事件循环主函数：epoll_wait 并分发事件（对齐 eventLoop）
    void EventLoop();

    // 处理新连接：accept 并初始化 HttpConn（LT 一次 / ET 循环，对齐 dealclientdata）
    void DealWithListen();

    // 处理连接可读事件：读请求、解析并响应（对齐 dealwithread 简化版）
    void DealWithRead(int fd);

    // 处理连接可写事件：继续发送未完成的响应（对齐 dealwithwrite）
    void DealWithWrite(int fd);

    const Config& config_;  // 服务器配置
    int listen_fd_;         // 监听 socket
    int epoll_fd_;          // epoll 实例
    bool listen_et_;        // 监听触发模式：true ET / false LT
    bool conn_et_;          // 连接触发模式：true ET / false LT
    std::string root_dir_;  // 静态资源根目录（绝对路径）
    HttpConn* users_;       // 连接数组（fd 索引）
    ThreadPool* thread_pool_;  // 线程池（任务自动分配与并发处理）
};
