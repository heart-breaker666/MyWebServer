# MyWebServer

基于 C++ 的高并发 Web 服务器，使用 epoll + 线程池实现，支持 LT/ET 触发模式和半同步半异步 / 多线程 Reactor 两种并发模型。

## 命令行参数

| 参数 | 含义 |
|------|------|
| `-p` | 监听端口 |
| `-t` | 线程池线程数 |
| `-a` | 并发模型：`0` = 半同步半异步，`1` = 多线程 Reactor |
| `-m` | 触发模式组合：`0` = LT+LT，`1` = LT+ET，`2` = ET+LT，`3` = ET+ET |
| `-l` | 日志写入方式：`0` = 同步，`1` = 异步 |
| `-s` | 数据库连接池大小 |
| `-c` | 是否关闭日志：`0` = 否，`1` = 是 |

## 压测结果

使用 `wrk -t8 -c1000 -d60s http://127.0.0.1:9006/` 进行压测，测试 1 分钟、8 线程、1000 连接。

### 半同步半异步模型（-a 0）

#### LT+LT（-m 0）

![a0_m0](docs/images/a0_m0.png)

#### LT+ET（-m 1）

![a0_m1](docs/images/a0_m1.png)

#### ET+LT（-m 2）

![a0_m2](docs/images/a0_m2.png)

#### ET+ET（-m 3）

![a0_m3](docs/images/a0_m3.png)

### 多线程 Reactor 模型（-a 1）

#### LT+LT（-m 0）

![a1_m0](docs/images/a1_m0.png)
