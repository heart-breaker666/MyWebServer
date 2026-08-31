// HTTP 连接处理单元测试。
// 使用 socketpair 模拟客户端连接，验证主从状态机解析与响应构造：
//   GET 静态文件/首页、404、畸形请求 400、POST 注册登录 CGI（403 失败路径）、
//   半包分片请求、keep-alive 头解析。
#include "../include/HttpConn.h"
#include "../include/ConnectionPool.h"
#include "../include/Log.h"

#include <mysql/mysql.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

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

// 静态资源根目录（相对项目根目录）
const char* kRoot = "./root";

// 测试连接对：client 端用于收发，HttpConn 绑定 server 端
struct TestPair {
    int client_fd;
    HttpConn conn;
};

// 初始化一对 socketpair 连接
void InitPair(TestPair& pair, bool conn_et = false) {
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    pair.client_fd = fds[0];
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pair.conn.Init(fds[1], addr, kRoot, conn_et);
}

// 发送全部数据
void WriteAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
}

// 读取全部数据（直到连接关闭）
std::string ReadAll(int fd) {
    std::string out;
    char buf[8192];
    while (true) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

// 完整流程：发送请求 → HttpConn 读/处理/写 → 返回响应
std::string Exchange(TestPair& pair, const std::string& request) {
    WriteAll(pair.client_fd, request);
    if (pair.conn.Read()) {
        pair.conn.Process();
        pair.conn.Write();
    }
    pair.conn.Close();
    const std::string response = ReadAll(pair.client_fd);
    close(pair.client_fd);
    return response;
}

// 测试 1：GET / 返回首页 200
void TestGetRoot() {
    TestPair pair;
    InitPair(pair);
    const std::string response =
        Exchange(pair, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(response.find("登录") != std::string::npos);  // index.html 内容
    CHECK(response.find("Connection:keep-alive") != std::string::npos);
}

// 测试 2：GET 静态图片返回 200 与正确 Content-Type
void TestGetImage() {
    TestPair pair;
    InitPair(pair);
    const std::string response =
        Exchange(pair, "GET /picture.jpg HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(response.find("Content-Type:image/jpeg") != std::string::npos);
}

// 测试 3：GET 不存在的资源返回 404
void TestGet404() {
    TestPair pair;
    InitPair(pair);
    const std::string response =
        Exchange(pair, "GET /no_such_file HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(response.find("HTTP/1.1 404 Not Found") != std::string::npos);
}

// 测试 4：畸形请求（缺少 HTTP 版本）返回 400
void TestBadRequest() {
    TestPair pair;
    InitPair(pair);
    const std::string response =
        Exchange(pair, "GET /index.html\r\n\r\n");
    CHECK(response.find("HTTP/1.1 400 Bad Request") != std::string::npos);
}

// 测试 5：Connection: close 请求响应为 close（短连接）
void TestConnectionClose() {
    TestPair pair;
    InitPair(pair);
    const std::string response = Exchange(
        pair,
        "GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(response.find("Connection:close") != std::string::npos);
}

// 测试 5b：keep-alive 长连接复用——同一连接连续处理两个请求均返回 200
void TestKeepAliveReuse() {
    TestPair pair;
    InitPair(pair);
    const std::string request =
        "GET /index.html HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: keep-alive\r\n\r\n";
    // 第一个请求
    WriteAll(pair.client_fd, request);
    CHECK(pair.conn.Read());
    pair.conn.Process();
    CHECK(pair.conn.IsWriteState());
    CHECK(pair.conn.Write());
    CHECK(pair.conn.IsKeepAlive());
    pair.conn.Reuse();  // 模拟上层：发送完成后复用连接
    // 第二个请求复用同一连接
    WriteAll(pair.client_fd, request);
    CHECK(pair.conn.Read());
    pair.conn.Process();
    CHECK(pair.conn.IsWriteState());
    CHECK(pair.conn.Write());
    pair.conn.Close();
    const std::string response = ReadAll(pair.client_fd);
    close(pair.client_fd);
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(response.find("Connection:keep-alive") != std::string::npos);
}

// 测试 6：POST 注册/登录 CGI（用户表驱动）
void TestRegisterLogin() {
    // 清理测试用户，保证幂等
    {
        ConnectionRAII conn(ConnectionPool::GetInstance());
        mysql_query(conn.Get(),
                    "DELETE FROM user WHERE username='testuser'");
    }
    // 重新加载用户表
    HttpConn loader;
    loader.InitMysqlResult(ConnectionPool::GetInstance());

    // 注册成功 → 200（返回登录页）
    TestPair reg;
    InitPair(reg);
    const std::string body = "user=testuser&passwd=123456";
    const std::string reg_req =
        "POST /3 HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    const std::string reg_res = Exchange(reg, reg_req);
    CHECK(reg_res.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(reg_res.find("登录") != std::string::npos);  // 跳回登录页

    // 验证数据库密码完整存储（防止表单解析偏移 bug：123456 被截断为 3456）
    {
        ConnectionRAII conn(ConnectionPool::GetInstance());
        mysql_query(conn.Get(),
                    "SELECT passwd FROM user WHERE username='testuser'");
        MYSQL_RES* result = mysql_store_result(conn.Get());
        MYSQL_ROW row = mysql_fetch_row(result);
        CHECK(row != nullptr);
        if (row != nullptr) {
            CHECK(std::string(row[0]) == "123456");
        }
        mysql_free_result(result);
    }

    // 登录成功 → 200（返回资源请求页）
    TestPair login;
    InitPair(login);
    const std::string login_req =
        "POST /2 HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    const std::string login_res = Exchange(login, login_req);
    CHECK(login_res.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(login_res.find("欢迎回来") != std::string::npos);  // welcome.html

    // 密码错误 → 403（Content-Length 必须与 body 一致）
    TestPair bad;
    InitPair(bad);
    const std::string bad_body = "user=testuser&passwd=wrong";
    const std::string bad_req =
        "POST /2 HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
        std::to_string(bad_body.size()) + "\r\n\r\n" + bad_body;
    const std::string bad_res = Exchange(bad, bad_req);
    CHECK(bad_res.find("HTTP/1.1 403 Forbidden") != std::string::npos);

    // 重复注册 → 403
    TestPair dup;
    InitPair(dup);
    const std::string dup_res = Exchange(dup, reg_req);
    CHECK(dup_res.find("HTTP/1.1 403 Forbidden") != std::string::npos);
}

// 测试 7：半包分片请求——状态机等待补齐后再解析
void TestSplitRequest() {
    TestPair pair;
    InitPair(pair);
    const std::string request =
        "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // 第一次只发送请求行（不完整）
    const std::size_t first = request.find("\r\n") + 2;
    WriteAll(pair.client_fd, request.substr(0, first));
    CHECK(pair.conn.Read());
    pair.conn.Process();  // 行不完整，无响应构造

    // 第二次发送剩余请求头
    WriteAll(pair.client_fd, request.substr(first));
    CHECK(pair.conn.Read());
    pair.conn.Process();
    CHECK(pair.conn.Write());
    pair.conn.Close();

    const std::string response = ReadAll(pair.client_fd);
    close(pair.client_fd);
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);
}

}  // namespace

int main() {
    // 初始化日志与连接池（CGI 测试依赖）
    Log::GetInstance().Init("test_http.log", 0, 8192, 5000000, 0);
    ConnectionPool::GetInstance().Init("127.0.0.1", "root", "fxh668",
                                       "yourdb", 3306, 4, 0);

    // HttpConn 静态成员：创建 epoll 供连接注册
    HttpConn::epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    HttpConn::user_count_ = 0;

    TestGetRoot();
    TestGetImage();
    TestGet404();
    TestBadRequest();
    TestConnectionClose();
    TestKeepAliveReuse();
    TestRegisterLogin();
    TestSplitRequest();

    close(HttpConn::epoll_fd_);

    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
