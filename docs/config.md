# MyWebServer 配置模块说明（轻量版）

配置模块通过**命令行参数**（`getopt` 短选项）配置服务器运行参数，不依赖配置文件与环境变量。所有参数均有内置默认值。

## 1. 启动示例

```bash
./server                          # 全部使用默认值
./server -p 8080                  # 自定义端口
./server -p 8080 -t 16 -m 3       # 端口 + 线程数 + 全 ET 模式
```

## 2. 命令行参数一览

| 选项 | 参数 | 默认值 | 说明 |
|------|------|--------|------|
| `-p` | 整数 | 9006 | 监听端口号 |
| `-l` | 0/1 | 0 | 日志写入方式：0 同步，1 异步 |
| `-m` | 0-3 | 0 | 触发组合模式（见下表） |
| `-s` | 整数 | 8 | 数据库连接池数量 |
| `-t` | 整数 | 8 | 线程池线程数量 |
| `-c` | 0/1 | 0 | 是否关闭日志：0 否，1 是 |
| `-a` | 0/1 | 0 | 并发模型：0 半同步半异步，1 多线程 reactor |

## 3. 触发组合模式（-m）

| 值 | listenfd | connfd |
|----|----------|--------|
| 0 | LT 水平触发 | LT 水平触发 |
| 1 | LT 水平触发 | ET 边缘触发 |
| 2 | ET 边缘触发 | LT 水平触发 |
| 3 | ET 边缘触发 | ET 边缘触发 |

## 4. 使用示例

```cpp
#include "../include/Config.h"

int main(int argc, char* argv[]) {
    Config config;
    config.ParseArg(argc, argv);   // 解析命令行参数

    int port = config.GetPort();
    bool async_log = config.IsAsyncLog();
    bool listenfd_et = config.IsListenfdET();
    bool connfd_et = config.IsConnfdET();
    int sql_pool_size = config.GetSqlPoolSize();
    int thread_pool_size = config.GetThreadPoolSize();
    bool close_log = config.GetCloseLog();
    bool reactor = config.IsReactorModel();
    // ...
}
```

访问接口说明：

- `GetPort()` — 监听端口号
- `IsAsyncLog()` — 日志是否异步写入（true 异步 / false 同步）
- `IsListenfdET()` / `IsConnfdET()` — 触发模式（true ET / false LT）
- `GetSqlPoolSize()` / `GetThreadPoolSize()` — 连接池/线程池数量
- `GetCloseLog()` — 是否关闭日志输出
- `IsReactorModel()` — 是否多线程 reactor 并发模型（true 多线程 reactor / false 半同步半异步）
