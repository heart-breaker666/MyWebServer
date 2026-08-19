#include "../include/HttpConn.h"

#include <mysql/mysql.h>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdarg>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

#include "../include/Log.h"

namespace {

// HTTP 响应状态文案
const char* kOk200Title = "OK";
const char* kError400Title = "Bad Request";
const char* kError400Form =
    "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* kError403Title = "Forbidden";
const char* kError403Form =
    "You do not have permission to get file from this server.\n";
const char* kError404Title = "Not Found";
const char* kError404Form =
    "The requested file was not found on this server.\n";
const char* kError500Title = "Internal Error";
const char* kError500Form =
    "There was an unusual problem serving the request file.\n";

// 用户表（用户名 -> 密码），注册登录校验用（对齐 TinyWebServer 全局 users）
std::map<std::string, std::string> users;
// 保护用户表
std::mutex users_mutex;

}  // namespace

int HttpConn::epoll_fd_ = -1;
std::atomic<int> HttpConn::user_count_{0};

HttpConn::HttpConn() : sockfd_(-1), conn_et_(false) {
    InitRequest();
}

void HttpConn::Init(int sockfd, const sockaddr_in& addr,
                    const std::string& root, bool conn_et) {
    sockfd_ = sockfd;
    addr_ = addr;
    root_ = root;
    conn_et_ = conn_et;

    // 注册到 epoll（按连接触发模式）
    // 使用 EPOLLONESHOT：事件触发一次后自动移出就绪列表，
    // 防止同批事件重复处理或 fd 重用后旧事件误关闭新连接（对齐 TinyWebServer）
    epoll_event event;
    event.events = EPOLLIN | EPOLLONESHOT;
    if (conn_et_) {
        event.events |= EPOLLET;
    }
    event.data.fd = sockfd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, sockfd_, &event);

    // 设置为非阻塞
    const int flags = fcntl(sockfd_, F_GETFL, 0);
    fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);

    ++user_count_;
    InitRequest();
}

void HttpConn::InitMysqlResult(ConnectionPool& pool) {
    ConnectionRAII conn(pool);
    if (mysql_query(conn.Get(), "SELECT username, passwd FROM user")) {
        LOG_ERROR("HttpConn: SELECT user failed: %s",
                  mysql_error(conn.Get()));
        return;
    }
    MYSQL_RES* result = mysql_store_result(conn.Get());
    if (result == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(users_mutex);
    users.clear();
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        users[row[0]] = row[1];
    }
    mysql_free_result(result);
}

void HttpConn::InitRequest() {
    // 重置解析状态机与各索引（对齐 TinyWebServer init()）
    check_state_ = kCheckRequestLine;
    linger_ = false;
    method_ = kGet;
    url_ = nullptr;
    version_ = nullptr;
    host_ = nullptr;
    content_length_ = 0;
    string_ = nullptr;
    cgi_ = 0;
    state_ = ProcessState::kStateRead;  // 初始处于读与处理阶段
    start_line_ = 0;
    checked_index_ = 0;
    read_index_ = 0;
    write_index_ = 0;
    file_address_ = nullptr;
    iv_count_ = 0;
    bytes_to_send_ = 0;
    bytes_have_send_ = 0;
    std::memset(read_buf_, '\0', kReadBufferSize);
    std::memset(write_buf_, '\0', kWriteBufferSize);
    std::memset(real_file_, '\0', kFileNameLen);
}

bool HttpConn::Read() {
    if (read_index_ >= kReadBufferSize) {
        return false;
    }
    if (!conn_et_) {
        // LT 模式：读一次即可（对齐 TinyWebServer read_once）。
        // LT 特性保证：只要内核缓冲还有未读数据，重新注册 EPOLLIN 后会持续触发，
        // 配合半包处理（ContinueRead 重新注册 EPOLLIN）继续读取，无需一次读空。
        const ssize_t bytes_read =
            recv(sockfd_, read_buf_ + read_index_, kReadBufferSize - read_index_, 0);
        if (bytes_read <= 0) {
            return false;  // 对端关闭或读失败
        }
        read_index_ += static_cast<int>(bytes_read);
        return true;
    }
    // ET 模式：事件只触发一次，必须循环读取直到 EAGAIN 一次取空缓冲
    while (true) {
        const ssize_t bytes_read =
            recv(sockfd_, read_buf_ + read_index_, kReadBufferSize - read_index_, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 数据读完
            }
            return false;
        }
        if (bytes_read == 0) {
            return false;  // 客户端关闭连接
        }
        read_index_ += static_cast<int>(bytes_read);
        if (read_index_ >= kReadBufferSize) {
            break;  // 读缓冲已满
        }
    }
    return read_index_ > 0;
}

HttpConn::LineStatus HttpConn::ParseLine() {
    // 从状态机：扫描缓冲切分一行，返回行状态
    for (; checked_index_ < read_index_; ++checked_index_) {
        const char temp = read_buf_[checked_index_];
        if (temp == '\r') {
            if ((checked_index_ + 1) == read_index_) {
                return kLineOpen;  // \r 位于末尾，等待下一字节
            }
            if (read_buf_[checked_index_ + 1] == '\n') {
                read_buf_[checked_index_++] = '\0';
                read_buf_[checked_index_++] = '\0';
                return kLineOk;
            }
            return kLineBad;
        }
        if (temp == '\n') {
            if (checked_index_ > 1 && read_buf_[checked_index_ - 1] == '\r') {
                read_buf_[checked_index_ - 1] = '\0';
                read_buf_[checked_index_++] = '\0';
                return kLineOk;
            }
            return kLineBad;
        }
    }
    return kLineOpen;
}

HttpConn::HttpCode HttpConn::ParseRequestLine(char* text) {
    // 解析：方法 URL HTTP/1.1
    url_ = std::strpbrk(text, " \t");
    if (url_ == nullptr) {
        return kBadRequest;
    }
    *url_++ = '\0';
    const char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        method_ = kGet;
    } else if (strcasecmp(method, "POST") == 0) {
        method_ = kPost;
        cgi_ = 1;
    } else {
        return kBadRequest;
    }
    url_ += std::strspn(url_, " \t");
    version_ = std::strpbrk(url_, " \t");
    if (version_ == nullptr) {
        return kBadRequest;
    }
    *version_++ = '\0';
    version_ += std::strspn(version_, " \t");
    if (strcasecmp(version_, "HTTP/1.1") != 0) {
        return kBadRequest;
    }
    // 去掉 URL 中的协议前缀
    if (strncasecmp(url_, "http://", 7) == 0) {
        url_ += 7;
        url_ = std::strchr(url_, '/');
    }
    if (strncasecmp(url_, "https://", 8) == 0) {
        url_ += 8;
        url_ = std::strchr(url_, '/');
    }
    if (url_ == nullptr || url_[0] != '/') {
        return kBadRequest;
    }
    // URL 为 / 时默认首页
    if (std::strlen(url_) == 1) {
        std::strcat(url_, "index.html");
    }
    check_state_ = kCheckHeader;
    return kNoRequest;
}

HttpConn::HttpCode HttpConn::ParseHeader(char* text) {
    // 空行表示请求头结束
    if (text[0] == '\0') {
        if (content_length_ != 0) {
            check_state_ = kCheckContent;  // 有请求体，继续解析
            return kNoRequest;
        }
        return kGetRequest;  // 无请求体，请求解析完成
    }
    if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += std::strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            linger_ = true;
        }
    } else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += std::strspn(text, " \t");
        content_length_ = std::atoi(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += std::strspn(text, " \t");
        host_ = text;
    } else {
        LOG_INFO("HttpConn: unknown header: %s", text);
    }
    return kNoRequest;
}

HttpConn::HttpCode HttpConn::ParseContent(char* text) {
    // 请求体读取完整后才处理
    if (read_index_ >= content_length_ + checked_index_) {
        text[content_length_] = '\0';
        string_ = text;  // 保存 POST 数据
        return kGetRequest;
    }
    return kNoRequest;
}

HttpConn::HttpCode HttpConn::ProcessRead() {
    // 主状态机：驱动从状态机逐行解析，按状态推进
    LineStatus line_status = kLineOk;
    HttpCode ret = kNoRequest;
    char* text = nullptr;
    while ((check_state_ == kCheckContent && line_status == kLineOk) ||
           ((line_status = ParseLine()) == kLineOk)) {
        text = GetLine();
        start_line_ = checked_index_;
        switch (check_state_) {
        case kCheckRequestLine:
            ret = ParseRequestLine(text);
            if (ret == kBadRequest) {
                return kBadRequest;
            }
            break;
        case kCheckHeader:
            ret = ParseHeader(text);
            if (ret == kBadRequest) {
                return kBadRequest;
            }
            if (ret == kGetRequest) {
                return DoRequest();
            }
            break;
        case kCheckContent:
            ret = ParseContent(text);
            if (ret == kGetRequest) {
                return DoRequest();
            }
            line_status = kLineOpen;  // 请求体未完整，等待更多数据
            break;
        default:
            return kInternalError;
        }
    }
    return kNoRequest;
}

HttpConn::HttpCode HttpConn::DoRequest() {
    std::strcpy(real_file_, root_.c_str());
    const char* p = std::strrchr(url_, '/');
    if (p == nullptr) {
        return kBadRequest;
    }

    // CGI：POST 且 URL 为 /2（登录）或 /3（注册），对齐 TinyWebServer
    if (cgi_ == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 解析表单：user=xxx&passwd=yyy
        // 用 std::string 定位解析，避免固定偏移（&passwd= 为 8 字符）导致的截断错误
        std::string name;
        std::string password;
        const std::string body = string_;
        const std::size_t user_pos = body.find("user=");
        const std::size_t passwd_pos = body.find("&passwd=");
        if (user_pos == std::string::npos || passwd_pos == std::string::npos) {
            return kForbiddenRequest;  // 表单格式错误
        }
        name = body.substr(user_pos + 5, passwd_pos - user_pos - 5);
        password = body.substr(passwd_pos + 8);
        // 限制长度，防止超长输入（对齐 TinyWebServer 的 100 字符缓冲）
        if (name.size() > 99) {
            name.resize(99);
        }
        if (password.size() > 99) {
            password.resize(99);
        }

        if (*(p + 1) == '3') {
            // 注册：用户名不存在则插入数据库
            bool exists = false;
            {
                std::lock_guard<std::mutex> lock(users_mutex);
                exists = users.find(name) != users.end();
            }
            if (!exists && !name.empty()) {
                std::string sql = "INSERT INTO user(username, passwd) VALUES('";
                sql += name;
                sql += "', '";
                sql += password;
                sql += "')";
                ConnectionRAII conn(ConnectionPool::GetInstance());
                std::lock_guard<std::mutex> lock(users_mutex);
                if (mysql_query(conn.Get(), sql.c_str()) == 0) {
                    users[name] = password;
                    std::strcpy(url_, "/index.html");  // 注册成功回登录页
                } else {
                    return kForbiddenRequest;
                }
            } else {
                return kForbiddenRequest;  // 用户名已存在
            }
        } else if (*(p + 1) == '2') {
            // 登录：校验用户名密码
            std::lock_guard<std::mutex> lock(users_mutex);
            const auto it = users.find(name);
            if (it != users.end() && it->second == password) {
                std::strcpy(url_, "/welcome.html");  // 登录成功进资源页
            } else {
                return kForbiddenRequest;  // 登录失败
            }
        }
    }

    // 组装静态文件完整路径
    const int real_len = static_cast<int>(std::strlen(real_file_));
    std::strncpy(real_file_ + real_len, url_, kFileNameLen - real_len - 1);

    if (stat(real_file_, &file_stat_) < 0) {
        return kNoResource;
    }
    if (!(file_stat_.st_mode & S_IROTH)) {
        return kForbiddenRequest;
    }
    if (S_ISDIR(file_stat_.st_mode)) {
        return kBadRequest;
    }

    // mmap 映射文件供分散写发送
    const int fd = open(real_file_, O_RDONLY);
    if (fd < 0) {
        return kNoResource;
    }
    file_address_ = static_cast<char*>(
        mmap(0, file_stat_.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    return kFileRequest;
}

void HttpConn::Unmap() {
    if (file_address_ != nullptr) {
        munmap(file_address_, file_stat_.st_size);
        file_address_ = nullptr;
    }
}

void HttpConn::Process() {
    const HttpCode read_ret = ProcessRead();
    if (read_ret == kNoRequest) {
        return;  // 请求未完整，等待更多数据（上层关闭连接）
    }
    if (!ProcessWrite(read_ret)) {
        Close();
        return;
    }
    // 响应构造完成，进入发送阶段（对齐 TinyWebServer：process 后 m_state 置 1）
    SetWriteState();
}

bool HttpConn::Write() {
    if (bytes_to_send_ == 0) {
        return true;
    }
    // 分散写发送响应头与文件内容
    while (true) {
        const ssize_t n = writev(sockfd_, iv_, iv_count_);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区满：重新注册 EPOLLOUT（保留 EPOLLONESHOT）后立即返回，
                // 事件循环在 socket 可写时再次调用 Write() 继续发送（对齐 TinyWebServer）。
                // 连接从此刻起由事件循环接管，线程池工作线程立即释放。
                ModFd(EPOLLOUT);
                return true;
            }
            Unmap();
            return false;
        }
        bytes_have_send_ += static_cast<int>(n);
        bytes_to_send_ -= static_cast<int>(n);
        if (static_cast<std::size_t>(bytes_have_send_) >= iv_[0].iov_len) {
            // 响应头已发完，剩余为文件内容
            iv_[0].iov_len = 0;
            iv_[1].iov_base = file_address_ + (bytes_have_send_ - write_index_);
            iv_[1].iov_len = static_cast<std::size_t>(bytes_to_send_);
        } else {
            // 响应头未发完，继续推进
            iv_[0].iov_base = write_buf_ + bytes_have_send_;
            iv_[0].iov_len -= bytes_have_send_;
        }
        if (bytes_to_send_ <= 0) {
            Unmap();
            return true;
        }
    }
}

void HttpConn::ModFd(uint32_t events) {
    // 重新注册 epoll 事件（对齐 TinyWebServer modfd）：保留 EPOLLONESHOT，
    // 按连接触发模式决定是否保留边缘触发
    epoll_event event;
    event.events = events | EPOLLONESHOT;
    if (conn_et_) {
        event.events |= EPOLLET;
    }
    event.data.fd = sockfd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, sockfd_, &event);
}

void HttpConn::EnableWrite() {
    // proactor 模式：工作线程处理完请求后调用，注册 EPOLLOUT 把连接交还主线程，
    // 由事件循环的 EPOLLOUT 分支调用 Write() 发送响应（写操作集中在主线程）
    ModFd(EPOLLOUT);
}

void HttpConn::ContinueRead() {
    // 请求不完整（半包）：重新注册 EPOLLIN（保留 EPOLLONESHOT）等待更多数据。
    // LT 下未读数据会持续触发，ET 下新数据到达会再次触发（对齐 TinyWebServer modfd(EPOLLIN)）
    ModFd(EPOLLIN);
}

void HttpConn::Close() {
    if (sockfd_ > 0) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sockfd_, nullptr);
        close(sockfd_);
        sockfd_ = -1;
        --user_count_;
        Unmap();
    }
}

sockaddr_in* HttpConn::GetAddress() {
    return &addr_;
}

bool HttpConn::AddResponse(const char* format, ...) {
    if (write_index_ >= kWriteBufferSize) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    const int len = vsnprintf(write_buf_ + write_index_,
                              kWriteBufferSize - 1 - write_index_, format,
                              arg_list);
    va_end(arg_list);
    if (len < 0 || len >= kWriteBufferSize - 1 - write_index_) {
        return false;
    }
    write_index_ += len;
    return true;
}

bool HttpConn::AddStatusLine(int status, const char* title) {
    return AddResponse("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool HttpConn::AddHeaders(int content_length) {
    return AddContentLength(content_length) && AddLinger() && AddBlankLine();
}

bool HttpConn::AddContentType() {
    // 按文件扩展名返回 Content-Type
    const char* type = "application/octet-stream";
    const char* ext = std::strrchr(real_file_, '.');
    if (ext != nullptr) {
        if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
            type = "text/html; charset=utf-8";
        } else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
            type = "image/jpeg";
        } else if (strcmp(ext, ".png") == 0) {
            type = "image/png";
        } else if (strcmp(ext, ".gif") == 0) {
            type = "image/gif";
        } else if (strcmp(ext, ".mp4") == 0) {
            type = "video/mp4";
        } else if (strcmp(ext, ".css") == 0) {
            type = "text/css";
        } else if (strcmp(ext, ".js") == 0) {
            type = "application/javascript";
        } else if (strcmp(ext, ".txt") == 0) {
            type = "text/plain";
        }
    }
    return AddResponse("Content-Type:%s\r\n", type);
}

bool HttpConn::AddContentLength(int content_length) {
    return AddResponse("Content-Length:%d\r\n", content_length);
}

bool HttpConn::AddLinger() {
    // 现阶段服务器每请求处理完即关闭连接（DealWithRead 无条件 Close），
    // 响应头统一为 close，避免与 keep-alive 行为不一致导致连接复用混乱
    return AddResponse("Connection:%s\r\n", "close");
}

bool HttpConn::AddBlankLine() {
    return AddResponse("%s", "\r\n");
}

bool HttpConn::AddContent(const char* content) {
    return AddResponse("%s", content);
}

bool HttpConn::ProcessWrite(HttpCode ret) {
    switch (ret) {
    case kInternalError:
        AddStatusLine(500, kError500Title);
        AddHeaders(std::strlen(kError500Form));
        AddContent(kError500Form);
        break;
    case kBadRequest:
        AddStatusLine(400, kError400Title);
        AddHeaders(std::strlen(kError400Form));
        AddContent(kError400Form);
        break;
    case kForbiddenRequest:
        AddStatusLine(403, kError403Title);
        AddHeaders(std::strlen(kError403Form));
        AddContent(kError403Form);
        break;
    case kNoResource:
        AddStatusLine(404, kError404Title);
        AddHeaders(std::strlen(kError404Form));
        AddContent(kError404Form);
        break;
    case kFileRequest:
        AddStatusLine(200, kOk200Title);
        if (file_stat_.st_size != 0) {
            AddContentType();
            AddHeaders(file_stat_.st_size);
            // 分散写：响应头 + mmap 文件内容
            iv_[0].iov_base = write_buf_;
            iv_[0].iov_len = write_index_;
            iv_[1].iov_base = file_address_;
            iv_[1].iov_len = file_stat_.st_size;
            iv_count_ = 2;
            bytes_to_send_ = write_index_ + file_stat_.st_size;
            return true;
        }
        {
            // 空文件返回空 body
            const char* ok_string = "<html><body></body></html>";
            AddHeaders(std::strlen(ok_string));
            AddContent(ok_string);
            break;
        }
    default:
        return false;
    }
    // 非文件响应：单个写块（响应头 + body）
    iv_[0].iov_base = write_buf_;
    iv_[0].iov_len = write_index_;
    iv_count_ = 1;
    bytes_to_send_ = write_index_;
    return true;
}
