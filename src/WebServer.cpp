#include "../include/WebServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "../include/Log.h"

namespace {

// 最大连接数（对齐 TinyWebServer 的 MAX_FD）
const int kMaxFd = 65536;
// 每次 epoll_wait 最多返回的事件数
const int kMaxEvents = 1024;

// MySQL 连接信息（与本地环境一致）
const char* kDbUrl = "127.0.0.1";
const char* kDbUser = "root";
const char* kDbPassword = "fxh668";
const char* kDbName = "yourdb";
const int kDbPort = 3306;

}  // namespace

WebServer::WebServer(const Config& config)
    : config_(config),
      listen_fd_(-1),
      epoll_fd_(-1),
      listen_et_(false),
      conn_et_(false),
      thread_pool_(nullptr) {
    // 连接数组（fd 索引），对齐 TinyWebServer 的 users
    users_ = new HttpConn[kMaxFd];
    // 静态资源根目录：当前工作目录 + /root
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        root_dir_ = std::string(cwd) + "/root";
    } else {
        root_dir_ = "./root";
    }
    // 注意：构造函数不产生日志，Log 需在 Run() 中 InitLog 之后才能使用
}

WebServer::~WebServer() {
    delete[] users_;
    delete thread_pool_;
    if (listen_fd_ > 0) {
        close(listen_fd_);
    }
    if (epoll_fd_ > 0) {
        close(epoll_fd_);
    }
}

void WebServer::InitEventMode() {
    // 对齐 TinyWebServer trig_mode()：由 -m 组合决定 listen/conn 触发模式
    listen_et_ = config_.IsListenfdET();
    conn_et_ = config_.IsConnfdET();
    LOG_INFO("WebServer: event mode listenET=%d connET=%d", listen_et_ ? 1 : 0,
             conn_et_ ? 1 : 0);
}

void WebServer::InitLog() {
    // 初始化日志系统：-l 控制同步/异步，-c 控制是否关闭（对齐 log_write）
    Log::GetInstance().Init("ServerLog", config_.GetCloseLog() ? 1 : 0, 8192,
                            5000000, config_.IsAsyncLog() ? 1024 : 0);
    LOG_INFO("MyWebServer starting: port=%d threads=%d sql_pool=%d",
             config_.GetPort(), config_.GetThreadPoolSize(),
             config_.GetSqlPoolSize());
}

void WebServer::InitSqlPool() {
    // 初始化数据库连接池：-s 控制连接数量（对齐 sql_pool）
    ConnectionPool& pool = ConnectionPool::GetInstance();
    pool.Init(kDbUrl, kDbUser, kDbPassword, kDbName, kDbPort,
              config_.GetSqlPoolSize(), config_.GetCloseLog() ? 1 : 0);
    LOG_INFO("MyWebServer: connection pool ready, free=%d",
             pool.GetFreeConnCount());
    // 从数据库加载用户表，供注册/登录 CGI 校验（对齐 initmysql_result）
    users_[0].InitMysqlResult(pool);
    LOG_INFO("MyWebServer: user table loaded");
}

void WebServer::InitThreadPool() {
    // 初始化线程池：-t 控制线程数，-a 控制并发模型（对齐 thread_pool）
    thread_pool_ = new ThreadPool(
        config_.GetThreadPoolSize(), 10000,
        config_.IsReactorModel() ? 1 : 0);
    LOG_INFO("MyWebServer: thread pool ready, threads=%d reactor=%d",
             config_.GetThreadPoolSize(), config_.IsReactorModel() ? 1 : 0);
}

void WebServer::Run() {
    InitLog();
    InitSqlPool();
    InitEventMode();
    InitThreadPool();
    InitSocket();
    EventLoop();
}

void WebServer::InitSocket() {
    // 创建监听 socket（对齐 TinyWebServer eventListen）
    listen_fd_ = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("WebServer: socket create failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    // 端口复用，避免重启时 TIME_WAIT 导致绑定失败
    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定任意网卡的指定端口
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config_.GetPort());
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("WebServer: bind port %d failed: %s", config_.GetPort(),
                  strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (listen(listen_fd_, 1024) < 0) {
        LOG_ERROR("WebServer: listen failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    SetNonBlocking(listen_fd_);

    // 创建 epoll 实例，注册监听描述符，并告知 HttpConn 静态成员
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LOG_ERROR("WebServer: epoll_create failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    AddFd(listen_fd_, true);
    HttpConn::epoll_fd_ = epoll_fd_;
    HttpConn::user_count_ = 0;

    // 忽略 SIGPIPE，防止向已关闭连接写数据导致进程崩溃
    signal(SIGPIPE, SIG_IGN);
    LOG_INFO("WebServer: listening on port %d, root=%s", config_.GetPort(),
             root_dir_.c_str());
}

void WebServer::SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void WebServer::AddFd(int fd, bool is_listen) {
    epoll_event event;
    event.events = EPOLLIN;
    // 按配置决定是否边缘触发
    if (is_listen ? listen_et_ : conn_et_) {
        event.events |= EPOLLET;
    }
    event.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
    SetNonBlocking(fd);
}

void WebServer::EventLoop() {
    epoll_event events[kMaxEvents];
    while (true) {
        const int num = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
        if (num < 0 && errno != EINTR) {
            LOG_ERROR("WebServer: epoll_wait failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < num; ++i) {
            const int fd = events[i].data.fd;
            // 处理新连接（对齐 TinyWebServer eventLoop 分发顺序）
            if (fd == listen_fd_) {
                DealWithListen();
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 连接异常断开，关闭连接
                LOG_INFO("WebServer: close fd=%d", fd);
                users_[fd].Close();
            } else if (events[i].events & EPOLLIN) {
                DealWithRead(fd);
            } else if (events[i].events & EPOLLOUT) {
                // 发送缓冲区恢复可写，继续发送未完成的响应（对齐 TinyWebServer eventLoop）
                DealWithWrite(fd);
            }
        }
    }
}

void WebServer::DealWithListen() {
    // 对齐 TinyWebServer dealclientdata：LT 一次 accept，ET 循环 accept
    if (!listen_et_) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        const int connfd =
            accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr),
                   &addr_len);
        if (connfd < 0) {
            LOG_ERROR("WebServer: accept failed: %s", strerror(errno));
            return;
        }
        if (HttpConn::user_count_ >= kMaxFd) {
            close(connfd);
            return;
        }
        users_[connfd].Init(connfd, client_addr, root_dir_, conn_et_);
        LOG_INFO("WebServer: new connection fd=%d, total=%d", connfd,
                 HttpConn::user_count_.load());
        return;
    }
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        const int connfd =
            accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr),
                   &addr_len);
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 已处理完当前所有连接
            }
            LOG_ERROR("WebServer: accept failed: %s", strerror(errno));
            break;
        }
        if (HttpConn::user_count_ >= kMaxFd) {
            close(connfd);
            break;
        }
        users_[connfd].Init(connfd, client_addr, root_dir_, conn_et_);
        LOG_INFO("WebServer: new connection fd=%d, total=%d", connfd,
                 HttpConn::user_count_.load());
    }
}

void WebServer::DealWithRead(int fd) {
    // 提交连接到线程池处理（对齐 TinyWebServer dealwithread）：
    //   proactor：主线程读取数据，线程池负责处理（构造响应/发送/关闭）
    //   reactor：直接提交连接，线程池负责读取并处理
    if (!config_.IsReactorModel()) {
        if (users_[fd].Read() && thread_pool_->Append(&users_[fd])) {
            return;  // 任务已提交，连接生命周期由线程池接管
        }
    } else if (thread_pool_->Append(&users_[fd])) {
        return;
    }
    // 读取失败或任务队列满：关闭连接
    users_[fd].Close();
}

void WebServer::DealWithWrite(int fd) {
    // 发送缓冲区恢复可写，继续发送未完成的响应（对齐 TinyWebServer dealwithwrite）：
    //   reactor：发送任务送回线程池，由工作线程执行写（写操作也在 worker 内完成）
    //   proactor：主线程直接发送（写操作集中在主线程）
    if (config_.IsReactorModel()) {
        // 进入发送阶段（对齐 TinyWebServer：append 前置 m_state=1），交由线程池续写
        users_[fd].SetWriteState();
        if (thread_pool_->Append(&users_[fd])) {
            return;  // 已提交，连接生命周期由线程池接管
        }
        users_[fd].Close();  // 任务队列满：关闭连接
        return;
    }
    // proactor：发送失败或已全部发完则关闭连接；
    // 仍未发完：Write() 内部已重新注册 EPOLLOUT，等待下次可写事件。
    if (!users_[fd].Write() || users_[fd].WriteDone()) {
        users_[fd].Close();
    }
}
