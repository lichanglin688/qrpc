# qrpc

基于 Qt6 `QLocalSocket`（本机命名管道 / Unix domain socket）的强类型 RPC 库，构建为动态库。

## 快速使用

服务端：注册函数 + `listen`（注册必须早于 `listen`）。业务函数返回 `T` 就是同步业务；
返回 `QFuture<T>` 则是异步业务，针对会阻塞的工作（IO、等设备、长计算）。

```cpp
#include <qrpc/rpc_server.h>
#include <QPromise>
#include <QThread>
#include <thread>

int add(int a, int b) { return a + b; }

// 返回 QFuture<T>：自己起线程干活，函数立刻返回，服务端线程不等待
QFuture<int> asyncAdd(int a, int b)
{
    auto promise = std::make_shared<QPromise<int>>();
    QFuture<int> future = promise->future();
    promise->start();

    std::thread([promise, a, b] {
        QThread::msleep(500);              // 阻塞式业务
        promise->addResult(a + b);
        promise->finish();                 // 必须 finish，否则永不回包
    }).detach();

    return future;
}

RpcServer server;
//server.setThreadCount(4);
//server.setExecPolicy(RpcServer::ExecPolicy::ThreadPool);
server.registerHandler<add>();
server.registerHandler<asyncAdd>();     // 与同步函数共用同一套注册接口
server.listen("my_rpc_server");        // 名字即端点名
```

典型场景：文件/数据库读写、串口与设备等待、图像或信号处理、耗时计算——凡是会阻塞或耗时较长的
工作都适合放进 `QFuture`，函数立刻返回，服务端线程不等待，future 完成后 qrpc 自动把结果回给客户端。

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

// 异步业务对客户端没有区别：R 由 QFuture<int> 推导为 int
client.callAsync<asyncAdd>(7, 8, [](const RpcResult<int>& r) {
    if (r.ok)
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

自定义数据结构：参数和返回值只要是「可默认构造 + 实现了 `QDataStream` 的 `operator<<` / `operator>>`」
就能直接传，注册与调用都不需要额外包装。

```cpp
struct Position
{
    double x = 0;
    double y = 0;
};

// 两个自由函数就够了；放进类型定义的头文件，服务端与客户端包含同一份
QDataStream& operator<<(QDataStream& out, const Position& p) { return out << p.x << p.y; }
QDataStream& operator>>(QDataStream& in, Position& p) { return in >> p.x >> p.y; }

bool moveTo(int deviceId, const QString& axis, const Position& target);   // 参数直接用自定义类型

server.registerHandler<moveTo>();
client.callAsync<moveTo>(1, QStringLiteral("XY"), Position{10.5, 20.5},
                         [](const RpcResult<bool>& r) { qDebug() << r.ok << r.value; });
```

- 返回值同理：`Position currentPosition();` 也可以直接注册；
- 字段的声明顺序就是序列化顺序，收发两端必须用同一份定义；改动结构会破坏兼容性（协议不带版本号）；
- Qt 容器（`QList` / `QVector` / `QMap` / `QHash`）以及 `std::pair`，只要元素可序列化就能直接传；
  `std::vector` 这类标准库容器需要自己写对应的 `operator<<` / `operator>>`；
- 枚举按底层整型序列化，不需要额外处理。

集成到其他工程：

```cmake
find_package(qrpc REQUIRED)                     # 或 add_subdirectory(qrpc)
target_link_libraries(app PRIVATE qrpc::qrpc)
```

## 服务端线程模型

一句话：**socket 的读写与解帧永远在服务端线程；业务函数跑在哪条线程由执行策略决定；回包一律回到服务端线程再写。**

```
服务端线程（listen 所在线程，跑事件循环）
  readyRead → 解帧 ──┬─ Inline     : 直接在这里调用业务函数
                     └─ ThreadPool : 投递到自有线程池，由池线程调用业务函数
  业务完成（同步返回 / QFuture 完成）──► 队列调用切回服务端线程 ──► 写 socket

线程池（仅 ThreadPool 策略）：一次请求一个任务，只执行业务函数，不碰 socket
```

三条约束：

1. **回包线程统一**：业务可能在任意线程、任意时刻完成（`QFuture`），回包统一通过队列调用切回服务端线程再写，同一连接不会被多线程写。
2. **注册表与订阅表只在服务端线程维护**：注册表在 `listen` 前完成、之后只读，池线程并发查表无需加锁；业务函数自己访问的共享状态需自行保证线程安全。
3. **错误不回包**：业务抛异常或 `QFuture` 失败都不会回包，客户端由自己的超时判定 `error == "timeout"`；要传错误请把错误放进返回值。

### 执行策略：Inline（单线程）与 ThreadPool

差异只有一个：业务函数在哪条线程被调用。

**`ExecPolicy::Inline`（默认，单线程）** —— 服务端线程一条线程干完所有事：收包、业务、回包依次完成，没有队列、没有跨线程切换，时序最简单。代价是排他：业务执行期间这条线程被独占，阻塞业务会把所有连接一起拖住，所以阻塞工作要么交给 `QFuture` 里的线程，要么改用 `ThreadPool`。适合轻量、非阻塞的业务。

**`ExecPolicy::ThreadPool`** —— 一次请求一个池任务，业务在池线程并发执行；`setThreadCount(n)` 决定并发度（默认 `idealThreadCount() - 1`）。适合阻塞 IO、CPU 密集、耗时不可预期的业务。

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
