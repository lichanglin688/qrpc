#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>
#include <QtEndian>

// 帧 = quint32(大端, body 长度) + body，body 首字节为消息类型。
//   Request     : type | id | method | args
//   Response    : type | id | payload
//   Push        : type | name | payload      （按主题推给订阅者）
//   Subscribe   : type | name                （客户端订阅主题）
//   Unsubscribe : type | name                （客户端取消订阅）
namespace rpc {

enum class MsgType : quint8
{
    Request = 0,
    Response = 1,
    Push = 2,
    Subscribe = 3,
    Unsubscribe = 4
};

constexpr int kHeaderSize = sizeof(quint32);

// 从累计缓冲区切出一个完整帧体；不足一帧返回 false（处理粘包/半包）
inline bool takeFrame(QByteArray& buffer, QByteArray& outBody)
{
    if (buffer.size() < kHeaderSize)
        return false;

    const quint32 len = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(buffer.constData()));

    // 先按"可用字节"比较，避免长度字段损坏时 total 溢出
    const int available = buffer.size() - kHeaderSize;
    if (static_cast<quint32>(available) < len)
        return false;

    outBody = buffer.mid(kHeaderSize, static_cast<int>(len));
    buffer.remove(0, kHeaderSize + static_cast<int>(len));
    return true;
}

// 只读首字节判断消息类型
inline MsgType peekType(const QByteArray& body)
{
    if (body.isEmpty())
        return static_cast<MsgType>(0);
    return static_cast<MsgType>(static_cast<quint8>(body.at(0)));
}

// -------------------- 请求 --------------------

struct Request
{
    quint64 id = 0;
    QString method;
    QByteArray args;
};

inline QByteArray buildRequestFrame(const Request& r)
{
    QByteArray frame;
    QDataStream ds(&frame, QIODevice::WriteOnly);
    ds << quint32(0)   // 长度占位
       << quint8(MsgType::Request) << r.id << r.method << r.args;

    ds.device()->seek(0);   // 回到帧头，把真实长度写进去
    ds << quint32(frame.size() - kHeaderSize);

    return frame;
}

inline Request decodeRequest(const QByteArray& body)
{
    QDataStream ds(body);
    quint8 t = 0;
    Request r;
    ds >> t >> r.id >> r.method >> r.args;
    return r;
}

// -------------------- 响应 --------------------

struct Response
{
    quint64 id = 0;
    QByteArray payload;
};

inline QByteArray buildResponseFrame(const Response& r)
{
    QByteArray frame;
    QDataStream ds(&frame, QIODevice::WriteOnly);
    ds << quint32(0)   // 长度占位
       << quint8(MsgType::Response) << r.id << r.payload;

    ds.device()->seek(0);   // 回到帧头，把真实长度写进去
    ds << quint32(frame.size() - kHeaderSize);

    return frame;
}

inline Response decodeResponse(const QByteArray& body)
{
    QDataStream ds(body);
    quint8 t = 0;
    Response r;
    ds >> t >> r.id >> r.payload;
    return r;
}

// -------------------- 推送（服务端 -> 客户端，无需请求） --------------------

struct Push
{
    QString name;
    QByteArray payload;
};

inline QByteArray buildPushFrame(const Push& p)
{
    QByteArray frame;
    QDataStream ds(&frame, QIODevice::WriteOnly);
    ds << quint32(0)   // 长度占位
       << quint8(MsgType::Push) << p.name << p.payload;

    ds.device()->seek(0);   // 回到帧头，把真实长度写进去
    ds << quint32(frame.size() - kHeaderSize);

    return frame;
}

inline Push decodePush(const QByteArray& body)
{
    QDataStream ds(body);
    quint8 t = 0;
    Push p;
    ds >> t >> p.name >> p.payload;
    return p;
}

// -------------------- 订阅 / 取消订阅 --------------------

inline QByteArray buildTopicFrame(MsgType type, const QString& name)
{
    QByteArray frame;
    QDataStream ds(&frame, QIODevice::WriteOnly);
    ds << quint32(0)   // 长度占位
       << quint8(type) << name;

    ds.device()->seek(0);   // 回到帧头，把真实长度写进去
    ds << quint32(frame.size() - kHeaderSize);

    return frame;
}

inline QString decodeTopic(const QByteArray& body)
{
    QDataStream ds(body);
    quint8 t = 0;
    QString name;
    ds >> t >> name;
    return name;
}

} // namespace rpc
