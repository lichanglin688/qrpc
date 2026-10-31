// 序列化层：任意可 QDataStream 化的参数打包 / 解包成 tuple。

#include <QtTest>

#include <tuple>

#include <qrpc/rpc_serialize.h>

namespace {

struct Position
{
    double x = 0;
    double y = 0;

    friend bool operator==(const Position& a, const Position& b) noexcept
    {
        return a.x == b.x && a.y == b.y;
    }
};

QDataStream& operator<<(QDataStream& out, const Position& p)
{
    return out << p.x << p.y;
}

QDataStream& operator>>(QDataStream& in, Position& p)
{
    return in >> p.x >> p.y;
}

} // namespace

class TestSerialization : public QObject
{
    Q_OBJECT

private slots:
    void scalarRoundTrip()
    {
        const int value = 12345;

        const auto [out] = deserialize<int>(serialize(value));
        QCOMPARE(out, value);
    }

    void stringRoundTrip()
    {
        const QString value = QStringLiteral("你好，qrpc");

        const auto [out] = deserialize<QString>(serialize(value));
        QCOMPARE(out, value);
    }

    void customTypeRoundTrip()
    {
        const Position value{1.5, -2.25};

        const auto [out] = deserialize<Position>(serialize(value));
        QCOMPARE(out, value);
    }

    void multipleArgsRoundTrip()
    {
        const int deviceId = 3;
        const QString axis = QStringLiteral("Z");
        const Position target{10.0, 20.0};

        const QByteArray data = serialize(deviceId, axis, target);
        QVERIFY(!data.isEmpty());

        const auto [outDevice, outAxis, outTarget] =
            deserialize<int, QString, Position>(data);

        QCOMPARE(outDevice, deviceId);
        QCOMPARE(outAxis, axis);
        QCOMPARE(outTarget, target);
    }

    void emptyArgs()
    {
        const QByteArray data = serialize();
        QVERIFY(data.isEmpty());

        (void)deserialize<>(data);
        QVERIFY(true);
    }

    void trailingDataIsRejected()
    {
        QByteArray data = serialize(int(7));
        data.append(QByteArrayLiteral("\x01\x02"));

        QVERIFY_THROWS_EXCEPTION(std::runtime_error, deserialize<int>(data));
    }

    void tooFewTypesIsRejected()
    {
        const QByteArray data = serialize(int(1), int(2));

        // 按更少的类型去读：剩下的字节应被判定为多余数据
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, deserialize<int>(data));
    }
};

QTEST_GUILESS_MAIN(TestSerialization)

#include "test_serialization.moc"
