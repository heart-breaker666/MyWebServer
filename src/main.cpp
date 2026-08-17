// 高并发服务器入口文件：解析配置并启动 WebServer。
// 各模块（日志/数据库连接池/事件模式/监听 socket）的初始化均封装在 WebServer 内部。
#include "../include/Config.h"
#include "../include/WebServer.h"

int main(int argc, char* argv[]) {
    // 解析命令行配置（端口/日志/触发模式/并发数等）
    Config config;
    config.ParseArg(argc, argv);

    // 启动服务器（内部完成日志、连接池、socket/epoll 初始化并进入事件循环）
    WebServer server(config);
    server.Run();
    return 0;
}
