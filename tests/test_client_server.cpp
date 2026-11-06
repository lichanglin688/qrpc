// 端到端：同进程内起 RpcServer + RpcClient，覆盖调用、返回值、服务端异步业务、
// 超时、流水线并发、ThreadPool 策略下的跨线程回包，以及发布订阅。

#include <QtTest>

#include <QFuture>
#include <QPromise>
#include <QTimer>

#include <memory>
#include <vector>

#include <qrpc/rpc_client.h>
#include <qrpc/rpc_server.h>

// -------------------- 被测业务函数（服务端实现） --------------------

int csAdd(int a, int b)
{
    return a + b;
}

bool csFlag(bool value)
{
    return value;
}

QString csEcho(const QString& text)
{
    return text;
}

// 异步业务：50ms 后完成，客户端察觉不到，只是一次普通调用
QFuture<int> csAsyncAdd(int a, int b)
{
    auto promise = std::make_shared<QPromise<int>>();
    QFuture<int> future = promise->future();
    promise->start();

    QTimer::singleShot(50, qApp, [promise, a, b] {
        promise->addResult(a + b);
        promise->finish();
    });

    return future;
}

// 慢业务：500ms 才完成，用来触发客户端超时
QFuture<int> csSlowAdd(int a, int b)
{
    auto promise = std::make_shared<QPromise<int>>();
    QFuture<int> future = promise->future();
    promise->start();

    QTimer::singleShot(500, qApp, [promise, a, b] {
        promise->addResult(a + b);
        promise->finish();
    });

    return future;
}

// 主题签名标记：只提供「名字 + 参数类型」，不会被真正调用
void csProgress(const QString& task, int percent)
{
    Q_UNUSED(task);
    Q_UNUSED(percent);
}

class TestClientServer : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        m_name = QStringLiteral("qrpc_cs_") + QString::fromLatin1(QTest::currentTestFunction());

        m_server = std::make_unique<RpcServer>();
        m_server->registerHandler<csAdd>();
        m_server->registerHandler<csFlag>();
        m_server->registerHandler<csEcho>();
        m_server->registerHandler<csAsyncAdd>();
        m_server->registerHandler<csSlowAdd>();
        QVERIFY(m_server->listen(m_name));

        m_client = std::make_unique<RpcClient>();
        m_client->setTimeout(2000);
        connect(m_client.get(), &RpcClient::connected, this, [this] { m_connected = true; });
        m_client->connectToServer(m_name);

        QVERIFY(QTest::qWaitFor([this] { return m_connected; }, 5000));
    }

    void cleanup()
    {
        m_client.reset();
        m_server.reset();
        m_connected = false;
    }

    void syncCall()
    {
        int result = 0;
        bool arrived = false;

        m_client->callAsync<csAdd>(3, 4, [&](const RpcResult<int>& r) {
            arrived = r.ok;
            result = r.value;
        });

        QTRY_COMPARE(arrived, true);
        QCOMPARE(result, 7);
    }

    void variousReturnTypes()
    {
        bool flag = false;
        QString echo;
        int done = 0;

        m_client->callAsync<csFlag>(true, [&](const RpcResult<bool>& r) {
            flag = r.ok && r.value;
            ++done;
        });
        m_client->callAsync<csEcho>(QStringLiteral("qrpc"), [&](const RpcResult<QString>& r) {
            echo = r.value;
            ++done;
        });

        QTRY_COMPARE(done, 2);
        QCOMPARE(flag, true);
        QCOMPARE(echo, QStringLiteral("qrpc"));
    }

    void serverSideFutureIsTransparent()
    {
        int result = 0;
        bool arrived = false;

        m_client->callAsync<csAsyncAdd>(10, 5, [&](const RpcResult<int>& r) {
            arrived = r.ok;
            result = r.value;
        });

        QTRY_COMPARE(arrived, true);
        QCOMPARE(result, 15);
    }

    void timeoutIsReportedAsError()
    {
        m_client->setTimeout(100);   // 服务端要 500ms 才回

        RpcResult<int> captured;
        bool arrived = false;

        m_client->callAsync<csSlowAdd>(1, 2, [&](const RpcResult<int>& r) {
            captured = r;
            arrived = true;
        });

        QTRY_COMPARE(arrived, true);
        QVERIFY(!captured.ok);
        QCOMPARE(captured.error, QStringLiteral("timeout"));
    }

    void manyConcurrentCallsKeepTheirOwnResult()   // 流水线：一连接在途多请求
    {
        constexpr int count = 50;
        std::vector<int> results;
        results.reserve(count);

        for (int i = 0; i < count; ++i) {
            m_client->callAsync<csAdd>(i, i, [&, i](const RpcResult<int>& r) {
                if (r.ok && r.value == i + i)
                    results.push_back(r.value);
            });
        }

        QTRY_COMPARE(int(results.size()), count);
    }

    void threadPoolPolicyRepliesFromWorkerThread()
    {
        m_server->setExecPolicy(RpcServer::ExecPolicy::ThreadPool);
        m_server->setThreadCount(2);

        int done = 0;
        for (int i = 0; i < 20; ++i) {
            m_client->callAsync<csAdd>(i, 1, [this, &done, i](const RpcResult<int>& r) {
                QCOMPARE(r.ok, true);
                QCOMPARE(r.value, i + 1);   // 回包必须按 id 找回各自的请求
                ++done;
            });
        }

        QTRY_COMPARE(done, 20);
    }

    void responseToClosedConnectionIsDropped()
    {
        // 业务 500ms 才回；请求发出后立刻断开连接，服务端回包时连接已断开
        // （对象可能还没析构），回包必须被静默丢弃且不影响服务端
        m_client->callAsync<csSlowAdd>(1, 2, [](const RpcResult<int>&) {});
        m_client.reset();

        QTest::qWait(700);   // 覆盖服务端实际回包的时刻

        m_connected = false;
        m_client = std::make_unique<RpcClient>();
        connect(m_client.get(), &RpcClient::connected, this, [this] { m_connected = true; });
        m_client->connectToServer(m_name);
        QVERIFY(QTest::qWaitFor([this] { return m_connected; }, 5000));

        int value = 0;
        m_client->callAsync<csAdd>(2, 3, [&value](const RpcResult<int>& r) { value = r.value; });
        QTRY_COMPARE(value, 5);
    }

    void publishToSubscribers()
    {
        int percent = 0;
        QString task;

        m_client->subscribe<csProgress>([&](const QString& t, int p) {
            task = t;
            percent = p;
        });

        // 订阅帧要飞到服务端之后才算生效，publish 的返回值本身就是最好的同步点
        QTRY_COMPARE(m_server->publish<csProgress>(QStringLiteral("build"), 42), 1);
        QTRY_COMPARE(percent, 42);
        QCOMPARE(task, QStringLiteral("build"));
    }

    void publishByStringTopicReachesSameSubscribers()
    {
        int percent = 0;
        m_client->subscribe<csProgress>([&](const QString&, int p) { percent = p; });

        // 字符串入口与 publish<csProgress>() 用的是同一个主题名 "csProgress"
        QTRY_COMPARE(m_server->publish(QStringLiteral("csProgress"),
                                       QStringLiteral("build"), 100), 1);
        QTRY_COMPARE(percent, 100);
    }

    void unsubscribeStopsDelivery()
    {
        int calls = 0;
        m_client->subscribe<csProgress>([&](const QString&, int) { ++calls; });

        // publish 会重试，回调可能被触发多次：等帧全部送达后再取稳定基线
        QTRY_COMPARE(m_server->publish<csProgress>(QStringLiteral("build"), 10), 1);
        QTest::qWait(50);
        QVERIFY(calls >= 1);
        const int before = calls;

        m_client->unsubscribe<csProgress>();
        QTRY_COMPARE(m_server->publish<csProgress>(QStringLiteral("build"), 20), 0);
        QTest::qWait(50);
        QCOMPARE(calls, before);
    }

    void subscriptionSurvivesReconnect()
    {
        int received = 0;
        m_client->subscribe<csProgress>([&](const QString&, int) { ++received; });

        // publish 的重试本身也会投递，只断言"至少到过一条"
        QTRY_COMPARE(m_server->publish<csProgress>(QStringLiteral("build"), 1), 1);
        QTRY_VERIFY(received >= 1);

        // 换一条连接：订阅主题应在重连后自动重新订阅
        m_connected = false;
        m_client.reset();
        m_client = std::make_unique<RpcClient>();
        connect(m_client.get(), &RpcClient::connected, this, [this] { m_connected = true; });
        int receivedAfter = 0;
        m_client->subscribe<csProgress>([&](const QString&, int) { ++receivedAfter; });
        m_client->connectToServer(m_name);

        QVERIFY(QTest::qWaitFor([this] { return m_connected; }, 5000));

        // 旧连接的订阅可能还没被服务端清掉，因此把 publish 放进重试条件里
        QTRY_VERIFY(m_server->publish<csProgress>(QStringLiteral("build"), 2) >= 1
                    && receivedAfter >= 1);
    }

private:
    QString m_name;
    std::unique_ptr<RpcServer> m_server;
    std::unique_ptr<RpcClient> m_client;
    bool m_connected = false;
};

QTEST_GUILESS_MAIN(TestClientServer)

#include "test_client_server.moc"
