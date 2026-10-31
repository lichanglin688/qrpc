// main.cpp: 单进程、异步、基于 QLocalSocket 的 RPC 演示。
//  - 基础 API 是回调式：callAsync<fn>(args..., cb)；
//  - 想在回调之上用 QFuture，可以自己写一个适配层（见 callAsFuture）；
//  - 发布订阅：server.publish<fn>(args...) / subscribe<fn>(cb)（强类型），
//              也可用字符串主题 server.publish(topic, args...) / subscribe(topic, cb)；
//  - 资源释放在析构函数中显式完成。

#include <QCoreApplication>
#include <QDataStream>
#include <QFuture>
#include <QPromise>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <qrpc/rpc_client.h>
#include <qrpc/rpc_server.h>

// -------------------- 自定义类型（走 QDataStream 序列化） --------------------

struct Position
{
    double x = 0;
    double y = 0;
};

QDataStream& operator<<(QDataStream& out, const Position& p)
{
    return out << p.x << p.y;
}

QDataStream& operator>>(QDataStream& in, Position& p)
{
    return in >> p.x >> p.y;
}

// -------------------- 业务函数（服务端实现） --------------------
// 这些函数运行在 RpcServer 自有线程池的线程里，不是主线程。

static QString threadTag()
{
    return QStringLiteral("thread %1").arg(
        QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16));
}

static QString policyName(RpcServer::ExecPolicy policy)
{
    switch (policy) {
    case RpcServer::ExecPolicy::Inline:
        return QStringLiteral("Inline");
    case RpcServer::ExecPolicy::ThreadPool:
        return QStringLiteral("ThreadPool");
    }
    return QStringLiteral("Unknown");
}

int add(int a, int b)
{
    return a + b;
}

// 业务自身较慢（800ms 后才完成），用来配合客户端超时演示
QFuture<int> slowAdd(int a, int b)
{
    auto promise = std::make_shared<QPromise<int>>();
    QFuture<int> future = promise->future();
    promise->start();

    QTimer::singleShot(800, qApp, [promise, a, b] {
        promise->addResult(a + b);
        promise->finish();
    });

    return future;
}

bool moveTo(int deviceId, const QString& axis, const Position& target)
{
    QTextStream(stdout) << QString("[server %1] moveTo: deviceId=%2 axis=%3 target=(%4, %5)")
                               .arg(threadTag()).arg(deviceId).arg(axis)
                               .arg(target.x).arg(target.y)
                        << Qt::endl;
    return true;
}

void notify(const QString& message)
{
    QTextStream(stdout) << QString("[server %1] notify: %2").arg(threadTag(), message)
                        << Qt::endl;
}

// 返回 QFuture 的异步业务：服务端拿到 future 后立即释放执行线程，
// 等 future 完成再回包（客户端只看到普通的一次调用）
QFuture<int> asyncAdd(int a, int b)
{
    auto promise = std::make_shared<QPromise<int>>();
    QFuture<int> future = promise->future();
    promise->start();

    // 用 qApp 作 context：无论业务在哪个线程被调用，都在主线程事件循环里延迟完成
    QTimer::singleShot(200, qApp, [promise, a, b] {
        promise->addResult(a + b);
        promise->finish();
    });

    return future;
}

// 异步业务里出错：异常通过 future 传播成 RPC 错误回给客户端
QFuture<int> failingAsync(int value)
{
    auto promise = std::make_shared<QPromise<int>>();
    QFuture<int> future = promise->future();
    promise->start();

    QTimer::singleShot(120, qApp, [promise, value] {
        promise->setException(std::make_exception_ptr(
            std::runtime_error(QStringLiteral("async failed for %1").arg(value).toStdString())));
        promise->finish();
    });

    return future;
}

// 主题签名标记：只用来给强类型订阅/发布提供签名（不会被真正调用）。
// 用函数符号时主题名是 "progress"，与下面的字符串常量必须一致。
void progress(const QString& task, int percent)
{
    Q_UNUSED(task);
    Q_UNUSED(percent);
}

// 字符串主题入口用到的名字
const auto kTopicProgress = QStringLiteral("progress");

// -------------------- 可选：在回调 API 之上包出 QFuture --------------------
// 只是演示"用户自己想用 QFuture 时怎么做"，不是框架的一部分。
template<auto Function, typename... Args>
QFuture<UnwrapFutureT<typename FunctionTraits<decltype(Function)>::ReturnType>>
callAsFuture(RpcClient& client, Args&&... args)
{
    using R = UnwrapFutureT<typename FunctionTraits<decltype(Function)>::ReturnType>;

    auto promise = std::make_shared<QPromise<R>>();   // QPromise 不可复制，用 shared_ptr 交给回调持有
    QFuture<R> future = promise->future();
    promise->start();

    client.callAsync<Function>(
        std::forward<Args>(args)...,
        [promise](const RpcResult<R>& result) {
            if (!result.ok)
                promise->setException(std::make_exception_ptr(
                    std::runtime_error(result.error.toStdString())));
            else if constexpr (!std::is_void_v<R>)
                promise->addResult(result.value);
            promise->finish();
        });

    return future;
}

// -------------------- 演示 --------------------

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QString serverName = QStringLiteral("qrpc_demo_server");

    // ---- 服务端 ----
    // 执行策略可由命令行选择：demo.exe [inline|pool]
    const QString policyArg = (argc > 1) ? QString::fromLocal8Bit(argv[1]).toLower()
                                         : QStringLiteral("inline");

    RpcServer::ExecPolicy policy = RpcServer::ExecPolicy::Inline;
    if (policyArg == QLatin1String("pool"))
        policy = RpcServer::ExecPolicy::ThreadPool;

    RpcServer server;
    server.setExecPolicy(policy);
    server.setThreadCount(4);   // 线程池线程数
    server.registerHandler<add>();
    server.registerHandler<slowAdd>();
    server.registerHandler<moveTo>();
    server.registerHandler<notify>();
    server.registerHandler<asyncAdd>();
    server.registerHandler<failingAsync>();

    if (!server.listen(serverName)) {
        out << "server listen failed: " << serverName << Qt::endl;
        return 1;
    }
    out << "server listening on '" << serverName << "' (policy="
        << policyName(server.execPolicy()) << ", threads=" << server.threadCount()
        << ", main " << threadTag() << ")" << Qt::endl;

    // ---- 客户端 ----
    RpcClient client;
    client.setTimeout(300);

    // 强类型订阅：主题名与回调参数类型都来自 progress 的签名（参数写 auto 也行）
    client.subscribe<progress>([](auto task, auto percent) {
        QTextStream(stdout) << "  <publish> " << task << " = " << percent << "%" << Qt::endl;
    });

    QObject::connect(&client, &RpcClient::connected, [&client, &out] {
        out << "client connected, sending async requests (callback style)..." << Qt::endl;

        // 回调只有一种形态：const RpcResult<R>&
        client.callAsync<add>(10, 20, [&out](const RpcResult<int>& r) {
            if (!r.ok) {
                out << "  <add> failed: " << r.error << Qt::endl;
                return;
            }
            out << "  <add> result = " << r.value << Qt::endl;
        });

        // 服务端业务返回 QFuture<int>：客户端不需要感知，仍是一次普通调用
        client.callAsync<asyncAdd>(7, 8, [&out](const RpcResult<int>& r) {
            if (!r.ok) {
                out << "  <asyncAdd> failed: " << r.error << Qt::endl;
                return;
            }
            out << "  <asyncAdd> result = " << r.value << " (server-side QFuture)" << Qt::endl;
        });

        // 异步业务失败时不回包，客户端按超时处理
        client.callAsync<failingAsync>(42, [&out](const RpcResult<int>& r) {
            out << "  <failingAsync> ok=" << r.ok << " error=" << r.error << Qt::endl;
        });

        // 服务端 800ms 才回，客户端 300ms 超时 -> error == "timeout"
        client.callAsync<slowAdd>(1, 2, [&out](const RpcResult<int>& r) {
            if (!r.ok)
                out << "  <slowAdd> failed: " << r.error << Qt::endl;
        });

        client.callAsync<moveTo>(1, QStringLiteral("XY"), Position{10.5, 20.5},
                                 [&out](const RpcResult<bool>& r) {
                                     out << "  <moveTo> ok=" << r.ok
                                         << " result = " << r.value << Qt::endl;
                                 });

        // 风格 B：用户自己想用 QFuture，就在回调之上包一层
        callAsFuture<notify>(client, QStringLiteral("hello from client"))
            .then([&out]() {
                out << "  <notify> done (via QFuture)" << Qt::endl;
            })
            .onFailed([&out](const std::runtime_error& e) {
                out << "  <notify> failed: " << e.what() << Qt::endl;
            });
    });

    QObject::connect(&client, &RpcClient::socketError, [&out](const QString& msg) {
        out << "socket error: " << msg << Qt::endl;
    });
    QObject::connect(&client, &RpcClient::pushUnhandled, [&out](const QString& name) {
        out << "  <pushUnhandled> " << name << Qt::endl;
    });

    client.connectToServer(serverName);

    // ---- 服务端发布：只发给订阅了该主题的客户端 ----
    // 强类型入口：主题名由 nameof 从 progress 推导
    QTimer::singleShot(300, &server, [&server, &out] {
        const int sent = server.publish<progress>(QStringLiteral("build"), 42);
        out << "  [server] publish<progress>(build, 42) -> " << sent
            << " subscriber(s)" << Qt::endl;
    });
    // 600ms 时取消订阅，之后服务端再发就该是 0 个订阅者
    QTimer::singleShot(600, &client, [&client, &out] {
        client.unsubscribe<progress>();
        out << "  [client] unsubscribe<progress>()" << Qt::endl;
    });
    // 字符串入口：主题名是普通字符串，可运行时拼接
    QTimer::singleShot(900, &server, [&server, &out] {
        const int sent = server.publish(kTopicProgress, QStringLiteral("build"), 100);
        out << "  [server] publish(\"" << kTopicProgress << "\", build, 100) -> " << sent
            << " subscriber(s)" << Qt::endl;
    });

    QTimer::singleShot(1500, &app, &QCoreApplication::quit);
    return app.exec();
}
