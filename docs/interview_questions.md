# MyWebServer 面试备战手册

> 基于本项目真实代码编写（`include/` + `src/`），每个问题给出【回答要点】与【追问深挖】。
> 答题原则：先讲结论 → 再讲原理 → 最后联系本项目代码。不要背稿，用"为什么"串起来。

---

## 一、项目自述模板（30 秒介绍）

> 我实现了一个基于 epoll 的 C++ 高并发 HTTP Web 服务器。核心是**单线程 epoll 事件循环 + 固定线程池**的 reactor 架构，支持半同步半异步和多线程 reactor 两种并发模型，通过命令行可切换。
>
> 主要模块包括：HTTP 请求解析（主从状态机）、线程池、惰性删除的小根堆定时器（用于超时清理非活跃连接）、同步/异步日志、MySQL 连接池、注册/登录 CGI 校验。静态文件通过 mmap + writev 分散写发送，支持 keep-alive 长连接。
>
> 项目中我也发现并修复过一些问题，比如数据竞争、目录穿越、SQL 注入等，对线程安全和 HTTP 协议细节有比较深的理解。

**讲的时候注意**：主动带出"我发现了什么问题、怎么解决的"，比单纯罗列功能更能加分。

---

## 二、架构与设计类（必问）

### Q1. 介绍一下你的服务器整体架构 / 一次请求从进入到返回的完整流程？

**回答要点**（按代码路径讲）：
1. `main` 解析参数（`Config::ParseArg`）→ `WebServer::Run()` 依次初始化日志、连接池、事件模式、线程池、socket/epoll（[WebServer.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/WebServer.cpp#L101-L108)）。
2. `EventLoop` 进入 `epoll_wait` 循环（[WebServer.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/WebServer.cpp#L171-L211)）：
   - 监听 fd 可读 → `DealWithListen` accept，`users_[fd].Init()` 注册 `EPOLLIN|EPOLLONESHOT`，并添加超时定时器。
   - 连接 fd 可读 → `DealWithRead`，顺延定时器后按模型分发：
     - 半同步半异步：主线程 `Read()`，成功后提交线程池做 `Process()`。
     - 多线程 reactor：直接把连接提交线程池，工作线程做 `Read()+Process()`。
3. 工作线程 `Process()` 用主从状态机解析请求（请求行→请求头→请求体），`DoRequest()` 处理 CGI 或映射静态文件，`ProcessWrite()` 构造响应（响应头 + mmap 文件）。
4. 写路径：reactor 模式注册 `EPOLLOUT` 送回线程池执行 `Write()`；半同步半异步由主线程 `DealWithWrite` 执行 `Write()`。
5. keep-alive：发送完成后 `Reuse()` 重置状态机重新注册 `EPOLLIN`；否则 `Close()`。

**追问深挖**：
- 为什么用 `EPOLLONESHOT`？（防止同批事件重复处理、fd 重用后旧事件误关新连接——见 [HttpConn.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/HttpConn.cpp#L58-L67) 注释）
- 两种模型的本质区别？（读写由谁做：主线程 or 工作线程）

### Q2. 为什么选择 epoll 而不是 poll/select？

**回答要点**：
- select：fd 集合用位图、上限 1024、每次调用要遍历全部 fd 且要把集合从用户态拷到内核态，O(n)。
- poll：链表存 fd、无数量限制，但仍需每次全量扫描 + 用户态/内核态拷贝。
- epoll：**红黑树注册 + 就绪链表**，只有就绪 fd 会返回（O(就绪数)）；`epoll_create` 后 `epoll_ctl` 增删改，`epoll_wait` 只取就绪事件，无需每次全量拷贝。

**追问深挖**：
- epoll 的水平触发（LT）和边缘触发（ET）区别？（ET 只在状态变化时触发一次，必须循环读写到 EAGAIN）
- 本项目为什么 ET/LT 可配？（`-m` 参数，`InitEventMode`）
- 惊群问题？多线程 epoll_wait 同一个实例？（本项目单线程循环不存在；扩展可用 `EPOLLEXCLUSIVE` 或 SO_REUSEPORT）

### Q3. 讲讲你的两种并发模型：半同步半异步 vs 多线程 reactor？

**回答要点**（对照代码 [ThreadPool.cpp](file:///home/fangdashuai/mycode/MyWebServer/include/ThreadPool.h#L101-L145) 的 `Run()`）：
- **半同步半异步**（`actor_model=0`）：主线程是"异步"部分（读写），线程池是"同步"部分（只做 Process）。读、写集中在主线程，串行无并发写问题，但**主线程容易被大文件写阻塞**。
- **多线程 reactor**（`actor_model=1`）：工作线程按连接状态分发，`kStateRead` 做 `Read+Process`，`kStateWrite` 做 `Write`；处理完注册 EPOLLOUT 交还事件循环再送回线程池写。读写分离到线程池，主线程只 accept 和分发。

**追问深挖**：
- 为什么半同步半异步写要交还主线程？（代码注释：写操作集中在主线程，避免多线程同时写同一连接）
- reactor 模式下读和写拆成两个任务，有什么代价？（任务增多、线程切换和锁竞争开销）
- 两种模式你压测过吗？哪种更好？（建议诚实回答：reactor 更接近主流，但要控制任务粒度）

---

## 三、HTTP 解析类（必问）

### Q4. 介绍一下你的 HTTP 请求解析——主从状态机？

**回答要点**：
- 主状态机三态：`kCheckRequestLine`（请求行）→ `kCheckHeader`（请求头）→ `kCheckContent`（请求体）（[HttpConn.h](file:///home/fangdashuai/mycode/MyWebServer/include/HttpConn.h#L32-L36)）。
- 从状态机 `ParseLine()` 逐字节切分一行（处理 `\r\n`，`\r` 在末尾返回 `kLineOpen` 等下一字节）。
- 好处：**不需要一次性收全请求**，天然支持 TCP 粘包/半包；`ProcessRead` 在数据不足时返回 `kNoRequest`，上层重新注册 `EPOLLIN` 等后续数据（`ContinueRead`）。

**追问深挖**：
- 遇到半包怎么办？（`kLineOpen` / `kNoRequest` → `ContinueRead()` 重新注册 EPOLLIN）
- 为什么要解析成 `\0` 分隔？（C 字符串操作方便，`strpbrk`/`strcasecmp` 直接用）
- 支持哪些方法？（代码只认 GET/POST，其他返回 `kBadRequest`）
- 请求体如何判断读完？（`ParseContent` 用 `read_index_ >= content_length_ + checked_index_` 判断）

### Q5. 讲一下 keep-alive 的实现？

**回答要点**：
- HTTP/1.1 默认 keep-alive（`linger_ = true`，除非 `Connection: close`）。
- 响应发送完成后 `Reuse()`：`InitRequest()` 重置状态机 + `ModFd(EPOLLIN)` 重新注册等待下一个请求（[HttpConn.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/HttpConn.cpp#L484-L488)）。
- 空闲超时由定时器兜底：无活动 15 秒后关闭。

**追问深挖**：
- keep-alive 和短连接哪个好？（复用 TCP 连接省去握手和 TIME_WAIT，但占 fd）
- 半关闭/管道破裂怎么处理？（忽略 SIGPIPE；`EPOLLRDHUP` 检测对端关闭）

### Q6. 静态文件是怎么发送的？什么是零拷贝？

**回答要点**：
- `DoRequest` 用 `open + mmap` 映射文件（[HttpConn.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/HttpConn.cpp#L393-L400)），`ProcessWrite` 构造两个 iovec（响应头 + 文件内容），`Write` 用 `writev` 一次性分散写。
- 好处：避免把文件读到用户态 buffer 再拷贝；`writev` 减少系统调用次数。
- 注意：**mmap + writev 不是严格的零拷贝**（数据仍经用户态写进内核 socket 缓冲），真正的零拷贝是 `sendfile`。这是本项目的可优化点。

**追问深挖**：
- `writev` 两个 iovec 的推进逻辑？（`bytes_have_send_ >= iv_[0].iov_len` 后只发文件部分，见 [HttpConn.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/HttpConn.cpp#L443-L452)）
- 发送缓冲区满怎么办？（EAGAIN → `ModFd(EPOLLOUT)` 注册写事件，下次可写再继续，不忙等）
- 为什么 mmap 要 MAP_PRIVATE？（写时拷贝，避免共享映射的同步开销）
- 如果让你优化，怎么做？（`sendfile` / `splice`；大文件支持 `Range` 分片）

---

## 四、epoll 事件驱动细节类

### Q7. 事件循环里定时器怎么和 epoll_wait 结合？为什么不用 alarm+SIGALRM？

**回答要点**：
- 每次 `epoll_wait` 前调用 `GetNextExpire()` 拿到最近到期时间，换算成毫秒作为 `epoll_wait` 的超时（[WebServer.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/WebServer.cpp#L175-L183)）。到期后 `Tick()` 返回过期 fd，统一关闭。
- 不用 signal 的原因：信号会打断事件循环（EINTR）、处理函数必须异步安全、还要管道传递，复杂度高；用 epoll_wait 超时更自然、无信号开销。

**追问深挖**：
- 定时器数据结构？（小根堆 `std::priority_queue`，堆顶最近到期）
- 惰性删除怎么做的？（每个 fd 维护版本号，重复注册/顺延时版本+1 压新节点；弹出时比对版本号丢弃旧节点，`CleanExpired`）
- 增删改查复杂度？（push O(logn)、取顶 O(1)、惰性删除 O(logn) 摊还）
- 为什么用秒级 `time_t`？（`kTimerTimeoutSec = 15`，秒级够用；更精确可用毫秒 + `CLOCK_MONOTONIC`）
- 一个 fd 在堆里可能有多少节点？（多个旧版本残留，靠 CleanExpired 清理）

### Q8. EPOLLONESHOT 的作用？加了之后要注意什么？

**回答要点**：
- 事件触发一次后自动从就绪队列移除，必须重新 `epoll_ctl(MOD)` 才能再次触发。
- 本项目作用：**防止同批事件重复处理**、防止 fd 重用后旧事件误操作新连接（[HttpConn.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/HttpConn.cpp#L58-L65)）。
- 注意：所有路径都要记得重新注册（`ContinueRead`/`Reuse`/`EnableWrite`/`Write` 中的 `ModFd(EPOLLOUT)`），漏了连接就"死"了。

**追问深挖**：
- 不用 ONESHOT 直接用 ET 行不行？（ET 本身只触发一次，但工作线程处理期间新数据到来会再次触发，可能同一连接同时被两个线程处理——ONESHOT 保证同一时刻只有一个线程碰这个连接）
- ONESHOT 的代价？（每次事件后多一次 `epoll_ctl` MOD 系统调用）

---

## 五、线程池类

### Q9. 线程池怎么实现的？任务队列满了怎么办？

**回答要点**：
- `std::queue<HttpConn*>` + `std::mutex` + `std::condition_variable`，FIFO；工作线程 `wait` 在有任务或 stop 时唤醒（[ThreadPool.h](file:///home/fangdashuai/mycode/MyWebServer/include/ThreadPool.h#L83-L97)）。
- 队列满（`max_requests=10000`）时 `Append` 返回 false → 上层关闭连接（[WebServer.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/WebServer.cpp#L268-L275)）。

**追问深挖**：
- 为什么用条件变量而不是自旋锁？（阻塞不占 CPU；任务队列适合条件变量）
- 队列满直接关连接合理吗？（粗暴；扩展可加背压/退避/动态扩容）
- 线程池停止时队列里还有任务怎么办？（`stop_ && empty()` 才退出，保证残留任务处理完，见 [ThreadPool.h](file:///home/fangdashuai/mycode/MyWebServer/include/ThreadPool.h#L92-L93)）
- 任务对象生命周期谁管？（主线程持有 `users_` 数组，连接归属固定，工作线程只操作指针——这也是本项目数据竞争问题的根源，可引出 Q14）

### Q10. 线程池为什么用裸指针传 HttpConn？为什么不用 std::function 任务？

**回答要点**：
- 固定任务类型省去 `std::function` 的堆分配和虚调用开销；指针传参避免拷贝。
- 局限：通用性差（这是学习项目权衡）。

**追问深挖**：
- 如果用 `std::function` 有什么好处和坏处？（灵活 vs 每次任务分配一次，性能差一些）
- 任务粒度考虑？（reactor 模式读写拆两个任务，任务太多锁竞争大；可考虑批量提交）

---

## 六、日志系统类

### Q11. 日志系统怎么做到高性能的？同步和异步的区别？

**回答要点**：
- 单例懒汉（magic static，线程安全）。
- 同步模式：`WriteLog` 内 `file_mutex_` 串行格式化并直接 `fputs + fflush`。
- 异步模式：格式化后压入有界 `std::deque<std::string>`，单后台线程 `AsyncLoop` 消费落盘（[Log.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/Log.cpp#L165-L183)）；队列满时**降级为同步写**保证不丢日志。

**追问深挖**：
- 为什么异步还要 fflush？（注释是"保证日志实时可见"，但这也是性能瓶颈，可优化为攒批落盘）
- 单例为什么线程安全？（C++11 magic static 局部静态变量初始化有标准保证）
- 跨天/按行数分割怎么做的？（`today_` 判断跨天；`log_count_ % split_lines_` 切序号文件）
- 怎么保证多线程下 buf_ 不互相覆盖？（`file_mutex_` 保护整个格式化）
- 项目的日志有没有问题？（每条 fflush + 全量 INFO + accept/close 都打日志 → 高并发瓶颈；可加级别过滤、去掉热路径日志）

---

## 七、数据库连接池类

### Q12. 连接池怎么实现的？为什么不用每次新建连接？

**回答要点**：
- 单例；`Init` 一次性建立 `max_conn` 条 MySQL 连接入队；`GetConnection` 空队列时条件变量阻塞等待；`ConnectionRAII` 析构自动归还（[ConnectionPool.h](file:///home/fangdashuai/mycode/MyWebServer/include/ConnectionPool.h#L64-L82)）。
- 好处：TCP 握手 + MySQL 认证开销大，复用连接显著降低延迟；控制数据库侧连接数上限。

**追问深挖**：
- 阻塞等连接会不会把工作线程卡死？（会——连接耗尽且 MySQL 慢时会阻塞；可加等待超时）
- 空闲连接被 MySQL `wait_timeout` 断开怎么办？（需心跳/定期 ping；本项目未做，是可改进点）
- RAII 的好处？（异常安全，不会泄漏连接）

### Q13. 用户注册/登录是怎么校验的？有什么安全风险？

**回答要点**：
- 启动时 `InitMysqlResult` 把 user 表加载到全局 map；注册拼 INSERT 并更新 map，登录查 map 比对（[HttpConn.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/HttpConn.cpp#L342-L375)）。
- 风险：SQL 拼接未转义（注入）、密码明文、全局 map 单锁。

**追问深挖**（主动暴露，展示你发现了问题）：
- SQL 注入怎么修？（预处理语句 `mysql_stmt` 或 `mysql_real_escape_string`）
- 密码怎么存？（加盐哈希如 bcrypt；比对用 `crypto` 库的 constant-time compare）
- map 锁竞争怎么优化？（读写锁、分片、或只读快照 + 原子切换）

---

## 八、并发与线程安全（加分项，面试官最想听）

### Q14. 你的服务器有没有线程安全问题？你发现了哪些？

**回答要点**（诚实 + 有深度，这是本项目最值得聊的点）：
1. **连接关闭与处理的竞态**：主线程 `Tick()` 超时直接 `users_[fd].Close()`，而工作线程可能正在 `Process/Write` 同一连接 —— 数据竞争。
2. **fd 重用**：`Close()` 里 `close(fd)` 后新连接复用该 fd，`Init()` 重置对象，旧工作线程继续写 → 响应串到新连接 / use-after-free。
3. 定时器跨线程：主线程 `AdjustTimer` 与工作线程 `Close` 里的 `DeleteTimer` 并发 —— 这一处已用互斥锁保护。

**解决思路**（回答方向）：
- 连接引用计数 / `shared_ptr<HttpConn>`，工作线程持引用，主线程关闭前等待引用归零。
- 关闭动作收敛到单一所有者（事件循环统一处理，超时只标记）。
- 每个连接一把小锁，或把连接的读写处理绑定到固定线程（真正多 reactor 每连接单线程，天然无竞争）。

**追问深挖**：
- `user_count_` 为什么用 atomic？（多线程并发 ++/--，[HttpConn.h](file:///home/fangdashuai/mycode/MyWebServer/include/HttpConn.h#L131)）
- 除了锁还有什么方案？（per-connection 线程绑定、无锁队列、原子引用计数）

### Q15. 你的锁都在哪？会不会成为瓶颈？

**回答要点**：
- 定时器互斥锁（主线程与工作线程并发）；线程池队列锁；日志 file_mutex_/queue_mutex_；连接池队列锁；用户表 users_mutex。
- 瓶颈分析：日志锁在 WriteLog 全路径持有（格式化+写文件），高并发日志是全局串行点；线程池单锁在任务洪峰下竞争明显；定时器锁每轮循环都加（已在上份分析中指出）。

**追问深挖**：
- 怎么定位锁竞争？（perf record / 火焰图看 `__lll_lock_wait` 占比）
- 有哪些减少锁的思路？（无锁队列、分片锁、per-thread buffer、原子操作替代）

---

## 九、项目缺陷与优化（面试官必问"你有什么不足"）

### Q16. 你觉得项目有哪些不足？如果重新做会怎么改进？

**回答要点**（挑 3 个讲透即可）：
1. **性能**：
   - 单线程事件循环是吞吐瓶颈 → 多 reactor / SO_REUSEPORT 多进程。
   - mmap+writev 非零拷贝 → `sendfile`。
   - `users_[65536]` 预分配 ~230MB → 按需分配/对象池。
   - 日志每条 fflush → 批量落盘 + 级别过滤。
2. **正确性**：连接生命周期竞态 → 引用计数统一管理（详见 Q14）。
3. **功能**：不支持 Range（视频不能拖动）、gzip、HEAD、URL 解码、chunked → 补全 HTTP 语义。
4. **工程化**：无配置文件、无优雅退出、无压测基线、编译未开 `-march=native`/LTO。

**追问深挖**（提前准备好数据）：
- 如果面试官问"你压测过吗？多少 QPS？"——**诚实回答**：项目带了 wrk 但没留基线数据，说明自己计划/已经怎么测（`wrk -t4 -c1000 -d10s http://ip:9006/index.html`，关注 Latency 与 QPS，对比 ET/LT、两种模型）。
- 为什么大文件用 sendfile 更好？（内核态拷贝，少一次用户态往返；配合 `TCP_CORK` 合并小响应头）

---

## 十、扩展进阶（体现"知其所以然"）

### Q17. 如果让你把服务器扩展到百万连接，你会怎么做？

**回答要点**（按层次讲）：
1. 内核：调大 `ulimit -n`、`/proc/sys/fs/file-max`、`net.core.somaxconn`；监听队列调大。
2. 架构：多进程 `fork + SO_REUSEPORT` 多 reactor；每个进程内可多线程共享 epoll（或每核一个 reactor 绑核）。
3. 连接内存：避免 `users_` 大数组，改为哈希/红黑树按需分配；读缓冲用小 buffer + 必要时扩容。
4. 事件驱动：epoll（1 万个 fd 内足够）；更大规模考虑 io_uring（内核驱动异步 IO，减少系统调用）。
5. 业务分离：静态文件走 nginx/CDN，本项目专注动态或学习用途。

**追问深挖**：
- `SO_REUSEPORT` 原理？（多个 socket 绑同端口，内核负载均衡分发到多进程，4.4+ 内核才支持真正均衡）
- 线程模型选型？（建议 1 个 reactor 线程负责 accept + 多个 worker 线程处理，或用 1:1 线程模型）

### Q18. 讲讲 C++ 层面的技术点（面试常考）

- **单例**：magic static 为何线程安全？（C++11 起局部静态变量初始化有内存序保证）
- **智能指针**：你项目里为什么还有裸指针 `users_`、`thread_pool_`？（学习项目，改进用 `unique_ptr`/`shared_ptr` 管理）
- **RAII**：`ConnectionRAII`、`lock_guard`、`unique_lock` 的用法与区别（unique_lock 可手动 unlock/lock，锁可迁移）。
- **移动语义**：`std::move` 在日志 `async_queue_.push_back(std::move(line))` 的用法（避免拷贝）。
- **条件变量**：为什么要配合 predicate 使用 `wait(lock, pred)`？（防止虚假唤醒/漏唤醒）
- **内存对齐/缓存**：`HttpConn` 对象被多线程访问的 cache line 问题（可选深聊）。
- **编译**：`-O2`、`-pthread`；`__attribute__((format(printf,...)))` 编译器检查格式串。

### Q19. 如果让你重写这个项目，架构上会怎么设计？

**回答要点**（展示设计能力）：
```
进程模型：master 管理 + N 个 worker（SO_REUSEPORT 或共享 listen fd）
worker 内：每 reactor 线程一个 epoll（连接按 fd 哈希绑定，单线程处理该连接读写）
任务分发：无锁 MPMC 队列 或 直接在线程内处理（无跨线程任务）
定时器：每个 reactor 私有小根堆 / 时间轮（毫秒级）
日志：per-thread 缓冲 + 批量落盘 + 级别过滤
连接：shared_ptr<HttpConn> 引用计数，杜绝 use-after-free
文件：sendfile 零拷贝 + Range 支持
```
**关键设计取舍**：每连接单线程处理 → 无需锁；跨线程只传"新任务事件"不传连接状态。

---

## 十一、一句话快答（压轴高频题）

| 问题 | 一句话答案 |
| --- | --- |
| ET 和 LT 选哪个？ | 都实现了；ET 效率高但要循环读写到 EAGAIN，LT 简单不易漏事件；本项目 `-m` 可配 |
| 为什么用线程池？ | 避免每个请求建线程的开销，控制并发上限，任务队列削峰 |
| 为什么用 epoll 不用多线程每连接一线程？ | fd 多时线程数爆炸、上下文切换开销大；epoll 用少量线程 + 事件驱动 |
| 怎么处理粘包半包？ | 状态机解析，`kLineOpen`/`kNoRequest` 等待更多数据 |
| 怎么处理客户端断开？ | `recv` 返回 0、`EPOLLRDHUP`、`EPOLLERR/HUP`；`SIGPIPE` 已忽略 |
| 大并发下写慢连接怎么办？ | 不阻塞：EAGAIN 后注册 EPOLLOUT 等可写再续写，非阻塞 socket |
| 你的定时器为什么不用时间轮？ | 本项目连接少、15s 超时粒度粗，小根堆 O(logn) 够用；连接极多时可换时间轮/层级时间轮 O(1) |
| 日志会不会丢？ | 异步队列满时降级同步写，宁可慢不丢 |
| 数据库挂了会怎样？ | 连接池阻塞等待，请求卡住——可加超时和熔断（改进点） |
| 项目最大的瓶颈？ | 单事件循环主线程 + 日志 fflush + 非零拷贝发送 |

---

## 十二、面试前自查清单

- [ ] 能对着 [WebServer.cpp](file:///home/fangdashuai/mycode/MyWebServer/src/WebServer.cpp#L171-L211) 手画事件循环流程
- [ ] 能讲清 `EPOLLONESHOT` + ET/LT 组合的四种模式
- [ ] 能画出主从状态机（请求行→头→体）的状态转移
- [ ] 能说出 `users_` 数组、线程池队列、定时器堆各自的数据结构与复杂度
- [ ] 能主动讲出 2~3 个项目缺陷及修复方案（数据竞争、目录穿越、SQL 注入、日志瓶颈、非零拷贝）
- [ ] 准备 1~2 个压测/性能数据（或说明计划怎么测）
- [ ] 会用一句话回答"项目最大的难点"（推荐：并发正确性——多线程共享连接对象的安全管理）
