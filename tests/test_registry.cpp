// 注册表：同步函数、void 函数、返回 QFuture 的异步函数，以及查表失败的分派行为。

#include <QtTest>

#include <QFuture>
#include <QPromise>
#include <QTimer>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

#include <qrpc/rpc_registry.h>
#include <qrpc/rpc_serialize.h>

// -------------------- 被测业务函数 --------------------

int add(int a, int b)
{
    return a + b;
}

QString concat(const QString& a, const QString& b)
{
    return a + b;
}

QStringList g_logged;   // 记录 void 函数的调用

void logging_notify(const QString& message)
{
    g_logged << message;
}

// 异步：50ms 后才给出结果，期间不占用调用线程
QFuture<int> laterAdd(int a, int b)
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

class TestRegistry : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        g_logged.clear();
    }

    void syncFunction()
    {
        RpcRegistry registry;
        registry.registerHandler<add>();

        bool done = false;
        QByteArray payload;

        registry.invoke(QStringLiteral("add"), serialize(20, 22),
                        [&](const QByteArray& p) { done = true; payload = p; });

        QVERIFY(done);
        QCOMPARE(std::get<0>(deserialize<int>(payload)), 42);
    }

    void voidFunction()
    {
        RpcRegistry registry;
        registry.registerHandler<logging_notify>();

        bool done = false;
        QByteArray payload(1, '\x7f');   // 期待被覆盖成空 payload

        registry.invoke(QStringLiteral("logging_notify"), serialize(QStringLiteral("hi")),
                        [&](const QByteArray& p) { done = true; payload = p; });

        QVERIFY(done);
        QVERIFY(payload.isEmpty());
        QCOMPARE(g_logged, QStringList{QStringLiteral("hi")});
    }

    void asyncFunction()
    {
        RpcRegistry registry;
        registry.registerHandler<laterAdd>();

        bool done = false;
        QByteArray payload;

        registry.invoke(QStringLiteral("laterAdd"), serialize(1, 2),
                        [&](const QByteArray& p) { done = true; payload = p; });

        QVERIFY(!done);   // future 未完成，此刻不应有回包
        QVERIFY(QTest::qWaitFor([&] { return done; }, 3000));
        QCOMPARE(std::get<0>(deserialize<int>(payload)), 3);
    }

    void stringReturnValue()
    {
        RpcRegistry registry;
        registry.registerHandler<concat>();

        QByteArray payload;
        registry.invoke(QStringLiteral("concat"),
                        serialize(QStringLiteral("qrpc"), QStringLiteral("-lib")),
                        [&](const QByteArray& p) { payload = p; });

        QCOMPARE(std::get<0>(deserialize<QString>(payload)), QStringLiteral("qrpc-lib"));
    }

    void unknownMethodThrows()
    {
        RpcRegistry registry;
        registry.registerHandler<add>();

        const QByteArray args = serialize(1, 2);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 registry.invoke(QStringLiteral("nope"), args, {}));
    }

    void duplicateRegistrationThrows()
    {
        RpcRegistry registry;
        registry.registerHandler<add>();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, registry.registerHandler<add>());
    }
};

QTEST_GUILESS_MAIN(TestRegistry)

#include "test_registry.moc"
