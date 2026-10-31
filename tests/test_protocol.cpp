// 帧协议：四种消息的编解码，以及 takeFrame 对粘包/半包的处理。

#include <QtTest>

#include <QDataStream>

#include <tuple>

#include <qrpc/rpc_protocol.h>

class TestProtocol : public QObject
{
    Q_OBJECT

private slots:
    void requestRoundTrip()
    {
        rpc::Request req;
        req.id = 0x1234567890ABCDEFull;
        req.method = QStringLiteral("namespace::add");
        req.args = QByteArrayLiteral("\x00\x01\x02\xff");

        const QByteArray frame = rpc::buildRequestFrame(req);
        QVERIFY(frame.size() > rpc::kHeaderSize);

        QByteArray buffer = frame;
        QByteArray body;
        QVERIFY(rpc::takeFrame(buffer, body));
        QVERIFY(buffer.isEmpty());
        QCOMPARE(rpc::peekType(body), rpc::MsgType::Request);

        const rpc::Request out = rpc::decodeRequest(body);
        QCOMPARE(out.id, req.id);
        QCOMPARE(out.method, req.method);
        QCOMPARE(out.args, req.args);
    }

    void responseRoundTrip()
    {
        rpc::Response resp;
        resp.id = 7;
        resp.payload = QByteArrayLiteral("payload");

        QByteArray buffer = rpc::buildResponseFrame(resp);
        QByteArray body;
        QVERIFY(rpc::takeFrame(buffer, body));
        QCOMPARE(rpc::peekType(body), rpc::MsgType::Response);

        const rpc::Response out = rpc::decodeResponse(body);
        QCOMPARE(out.id, quint64(7));
        QCOMPARE(out.payload, resp.payload);
    }

    void pushRoundTrip()
    {
        rpc::Push push;
        push.name = QStringLiteral("progress");
        push.payload = serializeArgs();

        QByteArray buffer = rpc::buildPushFrame(push);
        QByteArray body;
        QVERIFY(rpc::takeFrame(buffer, body));
        QCOMPARE(rpc::peekType(body), rpc::MsgType::Push);

        const rpc::Push out = rpc::decodePush(body);
        QCOMPARE(out.name, push.name);
        QCOMPARE(out.payload, push.payload);
    }

    void topicRoundTrip_data()
    {
        QTest::addColumn<int>("typeValue");

        QTest::newRow("subscribe") << int(rpc::MsgType::Subscribe);
        QTest::newRow("unsubscribe") << int(rpc::MsgType::Unsubscribe);
    }

    void topicRoundTrip()
    {
        QFETCH(int, typeValue);

        const auto type = static_cast<rpc::MsgType>(typeValue);
        const QString name = QStringLiteral("some/topic");

        QByteArray buffer = rpc::buildTopicFrame(type, name);
        QByteArray body;
        QVERIFY(rpc::takeFrame(buffer, body));
        QCOMPARE(rpc::peekType(body), type);
        QCOMPARE(rpc::decodeTopic(body), name);
    }

    void stickyFrames()
    {
        // 两帧首尾相接：takeFrame 必须一帧一帧取干净
        rpc::Request req;
        req.id = 1;
        req.method = QStringLiteral("first");
        rpc::Response resp;
        resp.id = 2;

        QByteArray buffer = rpc::buildRequestFrame(req);
        buffer.append(rpc::buildResponseFrame(resp));

        QByteArray body;
        QVERIFY(rpc::takeFrame(buffer, body));
        QCOMPARE(rpc::peekType(body), rpc::MsgType::Request);
        QCOMPARE(rpc::decodeRequest(body).method, QStringLiteral("first"));

        QVERIFY(rpc::takeFrame(buffer, body));
        QCOMPARE(rpc::peekType(body), rpc::MsgType::Response);
        QCOMPARE(rpc::decodeResponse(body).id, quint64(2));

        QVERIFY(!rpc::takeFrame(buffer, body));   // 取完后再无可取
    }

    void partialFrame()
    {
        rpc::Request req;
        req.id = 9;
        req.method = QStringLiteral("half");

        const QByteArray frame = rpc::buildRequestFrame(req);

        QByteArray body;
        for (int i = 1; i < frame.size(); ++i) {   // 逐字节补齐，只有最后一帧算完整
            QByteArray cut = frame.left(i);
            QVERIFY(!rpc::takeFrame(cut, body));
        }

        QVERIFY(rpc::takeFrame(const_cast<QByteArray&>(frame), body));
        QCOMPARE(rpc::decodeRequest(body).id, quint64(9));
    }

    void emptyBuffer()
    {
        QByteArray buffer;
        QByteArray body;
        QVERIFY(!rpc::takeFrame(buffer, body));

        QCOMPARE(rpc::peekType(QByteArray{}), static_cast<rpc::MsgType>(0));
    }

    void oversizedLengthIsNotConsumed()
    {
        // 长度字段声称 1GB，但实际只有头部可用：不能越界，也不能吃掉缓冲区
        QByteArray buffer(8, '\0');
        QDataStream ds(&buffer, QIODevice::WriteOnly);
        ds << quint32(0x40000000u) << quint32(0);

        QByteArray body;
        QVERIFY(!rpc::takeFrame(buffer, body));
        QCOMPARE(buffer.size(), 8);
    }

private:
    static QByteArray serializeArgs()
    {
        QByteArray data;
        QDataStream out(&data, QIODevice::WriteOnly);
        out << QStringLiteral("task") << 50;
        return data;
    }
};

QTEST_GUILESS_MAIN(TestProtocol)

#include "test_protocol.moc"
