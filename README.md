# qrpc

基于 Qt6 `QLocalSocket`（本机命名管道 / Unix domain socket）的强类型 RPC 库，构建为动态库。

## 快速使用

服务端：注册函数 + `listen`（注册必须早于 `listen`）。

```cpp
#include <qrpc/rpc_server.h>

int add(int a, int b) { return a + b; }

RpcServer server;
server.setThreadCount(4);
server.setExecPolicy(RpcServer::ExecPolicy::ThreadPool);
server.registerHandler<add>();
server.listen("my_rpc_server");     // 名字即端点名
```

业务函数也可以返回 `QFuture<T>`（异步业务）：注册方式与同步函数完全一样。这个能力针对的是
**会阻塞的业务**——磁盘/网络 IO、等外部设备、耗时的长计算等。写法是把真正的工作交给子线程，
函数本身立刻返回 future，服务端线程不被占用；future 完成后 qrpc 自动序列化结果并回包。
客户端无感知：`asyncAdd` 和 `add` 都是一次普通调用，结果类型同样按 `T` 处理。

```cpp
#include <QPromise>
#include <QThread>

// 返回值写成 QFuture<T> 即可；真正的耗时计算交给子线程
QFuture<int> asyncAdd(int a, int b)
{
    auto promise = std::make_shared<QPromise<int>>();   // QPromise 不可复制，用 shared_ptr 交给子线程
    QFuture<int> future = promise->future();
    promise->start();                                   // 必须先 start 才能往里写结果

    // 业务自己起一条一次性线程（std::thread 同理）
    QThread* thread = QThread::create([promise, a, b] {
        QThread::msleep(500);                           // 模拟阻塞式业务：IO、等设备、长计算
        promise->addResult(a + b);                      // 在工作线程里写结果
        promise->finish();                              // finish 之后 qrpc 才回包
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();

    return future;                                      // 立刻返回，不等待线程结束
}

server.registerHandler<asyncAdd>();      // 与同步函数共用同一套注册接口
```

自己起线程时有三个要点：`promise->start()` 必须先调；结果只能在子线程里通过 `addResult()` /
`setException()` 写入；**一定要 `finish()`**，否则 future 永远不完成、qrpc 永远不会回包，
客户端只会等到超时。不想自己管线程的话，`return QtConcurrent::run([...] { ... });` 也等价
（需要额外链接 `Qt6::Concurrent`）。

注意这里的线程和 `setExecPolicy()` 是两件事：执行策略决定「业务函数在哪条线程被调用」，
而 `QFuture` 里的线程是业务为了不阻塞调用方额外起的。两者不冲突——即使策略是 `Inline`
（单线程版本），业务函数依然可以自己开线程去干耗时活，服务端线程只管收发。

客户端调用与同步函数完全一致：`client.callAsync<asyncAdd>(7, 8, cb)`——`R` 由 `QFuture<int>`
推导为 `int`，回调仍然是 `void(const RpcResult<int>&)`，不需要任何额外处理。

注意：future 失败（抛异常）时服务端不回包，客户端按自己的超时判定 `error == "timeout"`；
需要把错误传回客户端，请把错误信息放进返回值里。详见「服务端处理逻辑」中的
`RpcRegistry::invoke` 分支。

客户端：`callAsync<函数>(参数..., 回调)`，回调固定是 `void(const RpcResult<R>&)`。

```cpp
#include <qrpc/rpc_client.h>

RpcClient client;
client.setTimeout(500);              // 0 表示不超时
client.connectToServer("my_rpc_server");

client.callAsync<add>(1, 2, [](const RpcResult<int>& r) {
    if (!r.ok) { /* r.error 目前只会是 "timeout" */ return; }
    qDebug() << r.value;
});
```

发布订阅：服务端按主题推送，客户端 `subscribe` 后自动（含断线重连）向服务端登记。

```cpp
void progress(const QString& task, int percent);   // 只作为签名标记，不会被调用

// 服务端：强类型入口（主题名 = "progress"），返回实际送达的客户端数
server.publish<progress>(QStringLiteral("build"), 42);

// 客户端
client.subscribe<progress>([](auto task, auto percent) { /* ... */ });
client.unsubscribe<progress>();
```

集成到其他工程：

```cmake
find_package(qrpc REQUIRED)                     # 或 add_subdirectory(qrpc)
target_link_libraries(app PRIVATE qrpc::qrpc)
```

## 服务端处理逻辑

线程模型只有两条原则：**socket 读写与解帧永远在服务端线程；业务函数按执行策略决定跑在哪**。

```
socket readyRead ──► 追加到该连接的接收缓冲 ──► takeFrame 循环切帧（粘包/半包）
                                                    │ 首字节 = 消息类型
        ┌───────────────────────────────────────────┼────────────────────────┐
     Request                                    Subscribe / Unsubscribe    其他
        │                                           │                        │
   dispatch(policy)                            更新订阅表                  丢弃
   ├─ Inline      : 当前线程直接执行
   └─ ThreadPool  : m_pool.start(一个 QRunnable)，池线程执行
        │
   RpcRegistry::invoke(method, args, reply)
        ├─ 查表失败            → 抛异常，被 execute() 吞掉，不回包
        ├─ 返回 void           → 调完立即 done(空 payload)
        ├─ 返回 T              → serialize(T) 后 done(payload)
        └─ 返回 QFuture<T>     → future.then(...)，完成时才 done(serialize(T))
        │
   completeRequest(): 当前线程 == 服务端线程 ? 直接写 : QMetaObject::invokeMethod 队列切回
        │
   writeResponse(): 连接仍在册则 socket->write(帧)，不 flush，交给事件循环合并写出
```

要点：

1. **注册表在 `listen` 前完成**，之后只读，池线程并发查表无需加锁。
2. **回包线程统一**：无论业务在哪个线程、`QFuture` 何时完成，写 socket 一定切回服务端线程，避免多线程写同一 socket。
3. **订阅表按连接维护**：连接断开时 `dropSubscriptions` 退订并 `deleteLater`，`publish` 只写仍在册的连接，返回实际写入数量；无人订阅直接丢弃。
4. **错误语义**：业务函数抛异常或 `QFuture` 失败都不会回包（异常不会逃逸到线程池），客户端由自己的超时定时器判定 `error == "timeout"`。需要传递错误信息请在返回值里表达。
5. **执行策略**：`Inline`（单线程）与 `ThreadPool`（线程池）的区别见下一节。
6. **析构**：停止接受连接 → `abort` 所有连接并断开回调 → `m_pool.clear()` + `waitForDone()` 等在跑的业务自然结束 → 清空容器。

客户端侧对应逻辑：每个请求分配递增 `id` 并登记 `Pending{handler, QBasicTimer}`；收到 `Response` 按 `id` 找回并停表，找不到（已超时）就丢弃；超时统一在 `timerEvent` 里回调 `ok=false, error="timeout"`；`connected` 时重发全部订阅。

### 执行策略：Inline（单线程）与 ThreadPool

`RpcServer` 有两种业务执行策略，差异只在「业务函数在哪条线程被调用」，socket 读写与解帧始终在服务端线程。

**`ExecPolicy::Inline` —— 单线程版本（默认）**

- 业务函数直接在服务端线程执行，也就是调用 `listen()`、跑着事件循环的那条线程；整个服务端只有这一条线程在工作。
- socket 读取 → 解帧 → 反序列化 → 业务 → 序列化 → `write` 回包，全在同一条线程里依次完成：没有队列、没有跨线程切换、不需要加锁，回调顺序与请求顺序天然一致，断点可以一路跟下去，是最轻、最好调试的形态。
- 不创建任何工作线程：`QThreadPool` 是懒启动的，`Inline` 下完全不碰它，`setThreadCount()` 也不产生线程。
- 代价是**排他**：业务函数执行期间这条线程被独占，其他连接的读取、其他请求的处理、回包统统排队。所以阻塞式业务（`sleep`、同步等待 IO）绝不能直接写在 `Inline` 的业务函数里：要么交给 `QFuture` 让子线程去跑（`examples/inline` 的 `asyncAdd` 用 `QtConcurrent::run` + `msleep`），要么干脆切成 `ThreadPool`。单进程 demo 里客户端和服务端还共用这条线程，服务端一阻塞，客户端自己的事件循环也会被卡住、所有请求一起超时。
- 适合：轻量、非阻塞、纯计算或只碰内存的业务；单连接、低并发场景；想用最少的线程和最简单的时序。

**`ExecPolicy::ThreadPool` —— 线程池版本**

- 一次请求 = 一个 `QRunnable` 投递进 `RpcServer` 自有的 `QThreadPool`，业务函数在池线程中并发执行，互不排队。
- 池线程**只跑业务函数**：收包、解帧、写 socket 仍然在服务端线程；业务完成后通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 把回包切回服务端线程再写，避免多线程同时写同一个 socket。
- `setThreadCount(n)` 设定池大小（默认 `idealThreadCount() - 1`，运行中调整也生效；`Inline` 下该值没有意义）。注册表在 `listen` 前完成、之后只读，所以池线程并发查表无需加锁，但业务函数自己访问的共享状态需要你自行保证线程安全。
- 代价：每个请求多一次任务投递 + 一次跨线程回包切换。
- 适合：有阻塞 IO、CPU 密集、耗时不可预期的业务；多连接、高并发场景。

| | `Inline`（单线程） | `ThreadPool` |
| --- | --- | --- |
| 业务执行线程 | 服务端线程 | 池线程（n 条） |
| 跨线程切换 | 无 | 派发 + 回包各一次 |
| 阻塞业务的影响 | 卡住整个服务端 | 只占住一个池线程 |
| 工作线程数 | 恒为 1 | `setThreadCount()` |
| 适用场景 | 轻量、非阻塞 | 阻塞、CPU 密集 |
| 对应示例 | `examples/inline` | `examples/pool` |

切换方式（`Inline` 是默认值，可以不用显式设置）：

```cpp
RpcServer server;
server.setThreadCount(4);                              // 先进程池相关配置
server.setExecPolicy(RpcServer::ExecPolicy::ThreadPool);
// 或
server.setExecPolicy(RpcServer::ExecPolicy::Inline);   // 单线程版本
```

无论选哪种策略，`QFuture` 形式的异步业务都能用：它不占用执行线程，两种策略下都只是把回包推迟到 future 完成时。

帧格式（`quint32` 大端长度 + body，body 首字节为类型）：

| 类型 | 布局 | 方向 |
| --- | --- | --- |
| `Request` (0) | `type \| id \| method \| args` | 客户端 → 服务端 |
| `Response` (1) | `type \| id \| payload` | 服务端 → 客户端 |
| `Push` (2) | `type \| name \| payload` | 服务端 → 订阅者 |
| `Subscribe` (3) / `Unsubscribe` (4) | `type \| name` | 客户端 → 服务端 |

## 运行示例与压测

```powershell
# 示例：Inline 策略（QFuture 业务跑在子线程、超时、发布订阅）
build/bin/Debug/qrpc_demo_inline.exe

# 示例：ThreadPool 策略（4 个池线程并发执行同步业务）
build/bin/Debug/qrpc_demo_pool.exe

# 压测：串行 / 流水线 / 多连接
build/bin/Debug/qrpc_bench.exe --policy pool --serial 2000 --conc 20000 --window 64 --clients 4 --threads 4
```

Windows 下运行前需保证 Qt 的 `bin` 目录在 `PATH` 中（或执行一次 `windeployqt`）。

## 说明与限制

- 只面向**同一台机器**的进程间通信（命名管道 / Unix domain socket），不是网络 RPC。
- 协议不携带版本号，通信双方需使用同一份代码构建。
- 传输格式依赖 `QDataStream::Qt_DefaultCompiledVersion`，跨 Qt 大版本升级时需同步两端。
