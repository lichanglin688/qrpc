// main.cpp: ThreadPool 策略示例（业务投递到服务端自有线程池，并发执行）。
//
// 这个示例只用同步业务函数：一次请求 = 一个池任务，多个请求可以被不同池线程
// 同时执行；socket 读写与帧解析始终留在服务端线程，回包通过队列调用切回去。
//
// 注意：这里刻意不放返回 QFuture 的业务函数（异步业务见 examples/inline）。

#include <QCoreApplication>
#include <QDataStream>
#include <QTextStream>
#include <QThread>
#include <QTimer>

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

// -------------------- 业务函数（服务端实现，全部是同步函数） --------------------
// 这些函数运行在 RpcServer 自有线程池的线程里，不是主线程。

static QString threadTag()
{
    return QStringLiteral("thread %1").arg(
        QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16));
}

int add(int a, int b)
{
    // 打印执行线程：并发调用会散落在池的不同线程上
    QTextStream(stdout) << QString("[server %1] add(%2, %3)").arg(threadTag()).arg(a).arg(b)
                        << Qt::endl;
    return a + b;
}

QString echo(const QString& text)
{
    return text;
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

// 主题签名标记：只用来给强类型订阅/发布提供签名（不会被真正调用）。
void progress(const QString& task, int percent)
{
    Q_UNUSED(task);
    Q_UNUSED(percent);
}

// -------------------- 演示 --------------------

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QString serverName = QStringLiteral("qrpc_demo_pool_server");

    // ---- 服务端：ThreadPool 策略 + 自己配置线程数 ----
    RpcServer server;
    server.setThreadCount(4);                              // 必须先设线程数
    server.setExecPolicy(RpcServer::ExecPolicy::ThreadPool);
    server.registerHandler<add>();
    server.registerHandler<echo>();
    server.registerHandler<moveTo>();
    server.registerHandler<notify>();

    if (!server.listen(serverName)) {
        out << "server listen failed: " << serverName << Qt::endl;
        return 1;
    }
    out << "server listening on '" << serverName << "' (policy=ThreadPool, threads="
        << server.threadCount() << ", main " << threadTag() << ")" << Qt::endl;

    // ---- 客户端 ----
    RpcClient client;
    int pending = 0;

    client.subscribe<progress>([](auto task, auto percent) {
        QTextStream(stdout) << "  <publish> " << task << " = " << percent << "%" << Qt::endl;
    });

    QObject::connect(&client, &RpcClient::connected, [&client, &out, &pending] {
        out << "client connected, sending concurrent requests..." << Qt::endl;

        // 连发 8 个请求：池里 4 个线程会分担这些同步任务
        constexpr int count = 8;
        pending = count;
        for (int i = 0; i < count; ++i) {
            client.callAsync<add>(i, i * 10, [&out, &pending](const RpcResult<int>& r) {
                out << "  <add> result = " << r.value << Qt::endl;
                --pending;
            });
        }

        client.callAsync<echo>(QStringLiteral("hello from client"),
                               [&out](const RpcResult<QString>& r) {
                                   out << "  <echo> result = " << r.value << Qt::endl;
                               });

        client.callAsync<moveTo>(1, QStringLiteral("XY"), Position{10.5, 20.5},
                                 [&out](const RpcResult<bool>& r) {
                                     out << "  <moveTo> ok=" << r.ok
                                         << " result = " << r.value << Qt::endl;
                                 });

        client.callAsync<notify>(QStringLiteral("pool is running"),
                                 [&out](const RpcResult<void>& r) {
                                     out << "  <notify> ok=" << r.ok << Qt::endl;
                                 });
    });

    QObject::connect(&client, &RpcClient::socketError, [&out](const QString& msg) {
        out << "socket error: " << msg << Qt::endl;
    });

    client.connectToServer(serverName);

    // ---- 发布订阅：服务端按主题推给订阅者 ----
    QTimer::singleShot(300, &server, [&server, &out] {
        const int sent = server.publish<progress>(QStringLiteral("build"), 42);
        out << "  [server] publish<progress>(build, 42) -> " << sent
            << " subscriber(s)" << Qt::endl;
    });
    QTimer::singleShot(600, &client, [&client, &out] {
        client.unsubscribe<progress>();
        out << "  [client] unsubscribe<progress>()" << Qt::endl;
    });
    QTimer::singleShot(900, &server, [&server, &out] {
        const int sent = server.publish<progress>(QStringLiteral("build"), 100);
        out << "  [server] publish<progress>(build, 100) -> " << sent
            << " subscriber(s)" << Qt::endl;
    });

    QTimer::singleShot(1200, &app, &QCoreApplication::quit);
    return app.exec();
}
