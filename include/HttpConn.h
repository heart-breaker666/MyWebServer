#pragma once

#include <atomic>
#include <cstdint>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <string>

#include "../include/ConnectionPool.h"

// HTTP 连接处理类（对齐 TinyWebServer 的 http_conn 设计）。
// 使用主从状态机解析 HTTP 报文：
//   从状态机 parse_line 逐行切分，主状态机按 请求行 → 请求头 → 请求体 推进；
// 使用 add_* 系列接口构造响应报文，静态文件通过 mmap + writev 分散写发送；
// POST 注册/登录通过数据库 user 表校验（前端 action=/2 登录、/3 注册）。
class HttpConn {
public:
    static constexpr int kFileNameLen = 200;     // 文件路径最长
    static constexpr int kReadBufferSize = 2048; // 读缓冲大小
    static constexpr int kWriteBufferSize = 1024;  // 写缓冲大小

    // 请求方法
    enum Method {
        kGet = 0, kPost, kHead, kPut, kDelete,
        kTrace, kOptions, kConnect, kPath
    };
    // 主状态机状态
    enum CheckState {
        kCheckRequestLine = 0,  // 解析请求行
        kCheckHeader,           // 解析请求头
        kCheckContent           // 解析请求体
    };
    // 解析结果
    enum HttpCode {
        kNoRequest,           // 请求不完整，继续等待
        kGetRequest,          // 请求解析完成
        kBadRequest,          // 请求报文语法错误
        kNoResource,          // 资源不存在
        kForbiddenRequest,    // 禁止访问
        kFileRequest,         // 请求静态文件
        kInternalError,       // 服务器内部错误
        kClosedConnection     // 连接关闭
    };
    // 从状态机（行解析）状态
    enum LineStatus {
        kLineOk = 0,  // 一行完整
        kLineBad,     // 行格式错误
        kLineOpen     // 行不完整
    };

    // 处理阶段（对齐 TinyWebServer m_state）：线程池按此决定执行何种操作
    enum class ProcessState {
        kStateRead = 0,   // 读与处理阶段：执行 Read/Process
        kStateWrite = 1   // 发送阶段：执行 Write
    };

    HttpConn();

    // 析构函数（virtual：线程池以基类指针调用，测试可派生覆写）
    virtual ~HttpConn() = default;

    // 初始化连接：保存 socket/地址/根目录，注册到 epoll 并置非阻塞
    // sockfd:  连接 socket；addr: 客户端地址；root: 静态资源根目录；
    // conn_et: 连接触发模式，true 边缘触发 / false 水平触发
    void Init(int sockfd, const sockaddr_in& addr, const std::string& root,
              bool conn_et);

    // 从数据库 user 表加载用户名密码映射（注册登录校验用）
    void InitMysqlResult(ConnectionPool& pool);

    // 读取客户端请求数据（循环读取，ET 读到 EAGAIN）
    virtual bool Read();

    // 处理请求：状态机解析 + 构造响应报文
    virtual void Process();

    // 发送响应数据（mmap 文件 + 响应头分散写）。
    // 对齐 TinyWebServer：发送缓冲区满时重新注册 EPOLLOUT（保留 EPOLLONESHOT）
    // 并立即返回，由事件循环在 socket 可写时再次调用本方法继续发送，不忙等占线程。
    virtual bool Write();

    // 查询响应是否已全部发送完成（线程池/事件循环据此决定关闭连接或等待下次 EPOLLOUT）
    bool WriteDone() const {
        return bytes_to_send_ <= 0;
    }

    // proactor 模式：工作线程处理完请求后调用，重新注册 EPOLLOUT，
    // 响应发送交由主线程事件循环驱动（actor 模型下写操作集中到主线程）
    void EnableWrite();

    // 请求不完整（半包）时调用：重新注册 EPOLLIN，等待客户端继续发送，
    // 下次数据到达后再次 Read/Process（对齐 TinyWebServer NO_REQUEST → modfd(EPOLLIN)）
    void ContinueRead();

    // 连接是否仍有效（sockfd 未关闭；Process 失败会内部 Close，用于区分半包与失败）
    bool IsConnected() const {
        return sockfd_ > 0;
    }

    // 当前是否处于发送阶段（线程池据此决定执行 Write 还是 Read/Process）
    bool IsWriteState() const {
        return state_ == ProcessState::kStateWrite;
    }

    // 进入发送阶段（对齐 TinyWebServer：process 完成后 m_state 置 1）
    void SetWriteState() {
        state_ = ProcessState::kStateWrite;
    }

    // 关闭连接并从 epoll 移除
    virtual void Close();

    // 获取客户端地址
    sockaddr_in* GetAddress();

    // epoll 实例（静态，供连接注册使用）
    static int epoll_fd_;
    // 当前活跃连接数（原子类型，多线程并发增删安全）
    static std::atomic<int> user_count_;

private:
    // 重置连接解析状态
    void InitRequest();

    // 从状态机：从缓冲解析出一行，返回行状态
    LineStatus ParseLine();

    // 获取当前待解析行的起始位置
    char* GetLine() { return read_buf_ + start_line_; }

    // 主状态机：按状态推进解析整个请求
    HttpCode ProcessRead();

    // 解析请求行（方法、URL、HTTP 版本）
    HttpCode ParseRequestLine(char* text);

    // 解析一个请求头字段
    HttpCode ParseHeader(char* text);

    // 解析请求体（POST 数据）
    HttpCode ParseContent(char* text);

    // 处理请求：CGI 注册/登录 与 静态文件映射
    HttpCode DoRequest();

    // 根据解析结果构造响应报文
    bool ProcessWrite(HttpCode ret);

    // 释放文件映射
    void Unmap();

    // 重新注册 epoll 事件（对齐 TinyWebServer modfd），Write 未完成时注册 EPOLLOUT
    void ModFd(uint32_t events);

    // 向写缓冲追加格式化响应内容
    bool AddResponse(const char* format, ...);

    // 追加响应体内容
    bool AddContent(const char* content);

    // 追加状态行
    bool AddStatusLine(int status, const char* title);

    // 追加响应头（长度 + 连接方式 + 空行）
    bool AddHeaders(int content_length);

    // 追加 Content-Type（按文件扩展名）
    bool AddContentType();

    // 追加 Content-Length
    bool AddContentLength(int content_length);

    // 追加 Connection
    bool AddLinger();

    // 追加空行
    bool AddBlankLine();

    int sockfd_;             // 连接 socket
    sockaddr_in addr_;       // 客户端地址
    std::string root_;       // 静态资源根目录
    bool conn_et_;           // 连接触发模式：true ET / false LT
    ProcessState state_;     // 处理阶段：读/处理 or 发送（对齐 TinyWebServer m_state）

    char read_buf_[kReadBufferSize];  // 读缓冲
    int read_index_;                  // 已读入字节数
    int checked_index_;               // 已检查字节数
    int start_line_;                  // 当前解析行起始位置
    char write_buf_[kWriteBufferSize];  // 写缓冲
    int write_index_;                 // 已写入字节数

    CheckState check_state_;          // 主状态机当前状态
    Method method_;                   // 请求方法
    char real_file_[kFileNameLen];    // 实际文件路径
    char* url_;                       // 请求 URL
    char* version_;                   // HTTP 版本
    char* host_;                      // Host 头
    int content_length_;              // 请求体长度
    bool linger_;                     // 是否保持连接
    char* string_;                    // POST 请求体内容
    int cgi_;                         // 是否 POST 请求

    char* file_address_;              // 静态文件 mmap 地址
    struct stat file_stat_;           // 静态文件信息
    struct iovec iv_[2];              // 分散写缓冲区数组
    int iv_count_;                    // 分散写块数
    int bytes_to_send_;               // 待发送总字节数
    int bytes_have_send_;             // 已发送字节数
};
