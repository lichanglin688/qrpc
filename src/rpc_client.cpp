#include <qrpc/rpc_client.h>

#include <QTimerEvent>

#include <qrpc/rpc_protocol.h>

RpcClient::RpcClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QLocalSocket::connected, this,
            [this] {
                // 连接（或重连）后把已订阅的主题重新订阅一次，再通知使用者
                for (const auto& entry : m_subscriptions)
                    sendTopic(rpc::MsgType::Subscribe, QString::fromStdString(entry.first));
                emit connected();
            });
    connect(&m_socket, &QLocalSocket::disconnected, this,
            [this] { emit disconnected(); });
    connect(&m_socket, &QLocalSocket::readyRead, this,
            [this] { onReadyRead(); });
    connect(&m_socket, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) { emit socketError(m_socket.errorString()); });
}

RpcClient::~RpcClient()
{
    // 显式释放：断开回调 -> 关闭连接 -> 清空待处理表。
    // 断开后，成员析构阶段不会再有回调访问本对象。
    m_socket.disconnect(this);
    m_socket.abort();

    // 显式停掉所有仍未触发的超时定时器，再清表
    for (auto& entry : m_pending) {
        if (entry.second.timer.isActive())
            entry.second.timer.stop();
    }
    m_pending.clear();
    m_timerIndex.clear();
    m_subscriptions.clear();
}

void RpcClient::connectToServer(const QString& name)
{
    m_socket.connectToServer(name);
}

void RpcClient::sendTopic(rpc::MsgType type, const QString& name)
{
    if (m_socket.state() != QLocalSocket::ConnectedState)
        return;   // 未连接：connected 时统一重发

    // 不 flush：交给事件循环合并写出（write() 会先尝试直接写，剩余的排队）
    m_socket.write(rpc::buildTopicFrame(type, name));
}

void RpcClient::sendRequest(quint64 id, const QString& method, const QByteArray& args)
{
    rpc::Request req;
    req.id = id;
    req.method = method;
    req.args = args;

    // 不 flush：及时写由事件循环负责，突发请求可以合并成更少的系统调用
    m_socket.write(rpc::buildRequestFrame(req));
}

void RpcClient::onReadyRead()
{
    m_buffer.append(m_socket.readAll());

    QByteArray body;
    while (rpc::takeFrame(m_buffer, body))
        handleFrame(body);
}

void RpcClient::handleFrame(const QByteArray& body)
{
    switch (rpc::peekType(body)) {
    case rpc::MsgType::Response:
        handleResponse(body);
        break;
    case rpc::MsgType::Push:
        handlePush(body);
        break;
    default:
        break;
    }
}

void RpcClient::handleResponse(const QByteArray& body)
{
    const rpc::Response resp = rpc::decodeResponse(body);

    const auto it = m_pending.find(resp.id);
    if (it == m_pending.end())
        return;   // 已超时或未知请求，丢弃

    // 收到响应：立刻关掉该请求的超时定时器，不再等它空转到期
    if (it->second.timer.isActive()) {
        m_timerIndex.erase(it->second.timer.timerId());
        it->second.timer.stop();
    }

    auto callback = std::move(it->second.handler);
    m_pending.erase(it);
    callback(true, resp.payload);
}

void RpcClient::timerEvent(QTimerEvent* event)
{
    const auto indexIt = m_timerIndex.find(event->timerId());
    if (indexIt == m_timerIndex.end()) {
        QObject::timerEvent(event);   // 不是本类的超时定时器
        return;
    }

    const quint64 id = indexIt->second;
    m_timerIndex.erase(indexIt);

    const auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;

    it->second.timer.stop();
    auto callback = std::move(it->second.handler);
    m_pending.erase(it);
    callback(false, QByteArrayLiteral("timeout"));
}

void RpcClient::handlePush(const QByteArray& body)
{
    const rpc::Push p = rpc::decodePush(body);

    const auto it = m_subscriptions.find(p.name.toStdString());
    if (it == m_subscriptions.end()) {
        emit pushUnhandled(p.name);
        return;
    }

    it->second(p.payload);
}
