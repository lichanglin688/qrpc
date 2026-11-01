#include <qrpc/rpc_server.h>

#include <QLocalSocket>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>

#include <algorithm>
#include <utility>

// ThreadPool 策略：一次请求 = 一个 QRunnable
class RpcServer::Task final : public QRunnable
{
public:
    Task(RpcServer* server, QPointer<QLocalSocket> socket, rpc::Request request)
        : m_server(server)
        , m_socket(std::move(socket))
        , m_request(std::move(request))
    {
        setAutoDelete(true);   // 由 QThreadPool 在 run() 返回后 delete
    }

    void run() override
    {
        // 业务可能返回 QFuture，完成回调可能会从任意线程触发，
        // 由 completeRequest 负责切回服务端线程。this 会在 run() 返回后被 delete，
        // 所以只捕获值（并用 QPointer 观察 server 是否还活着）。
        const QPointer<RpcServer> server = m_server;
        const QPointer<QLocalSocket> socket = m_socket;
        const quint64 id = m_request.id;

        m_server->execute(m_request, [server, socket, id](const QByteArray& payload) {
            if (!server)
                return;
            server->completeRequest(socket, rpc::Response{id, payload});
        });
    }

private:
    RpcServer* m_server;
    QPointer<QLocalSocket> m_socket;
    rpc::Request m_request;
};

RpcServer::RpcServer(QObject* parent)
    : QObject(parent)
{
    m_pool.setObjectName(QStringLiteral("RpcServerPool"));
    m_threadCount = std::max(1, QThread::idealThreadCount() - 1);
    // 池的线程数等切到 ThreadPool 策略时再配置，Inline 下完全不碰池

    connect(&m_server, &QLocalServer::newConnection,
            this, &RpcServer::onNewConnection);
}

RpcServer::~RpcServer()
{
    // 1) 停止接收新连接
    m_server.disconnect(this);

    // 2) 断开所有连接上的回调并关闭连接（m_buffers 的键就是全部在册连接）
    for (auto it = m_buffers.cbegin(); it != m_buffers.cend(); ++it) {
        it.key()->disconnect(this);
        it.key()->abort();
    }

    // 3) 丢弃排队任务，等待正在执行的业务函数自然结束
    m_pool.clear();
    m_pool.waitForDone();

    // 4) 清空容器
    m_buffers.clear();
    m_subscribers.clear();
}

bool RpcServer::listen(const QString& name)
{
    // 这里不再配置线程池：只有 ThreadPool 策略下 setExecPolicy/setThreadCount 才配置，
    // Inline 策略完全不会创建池线程（QThreadPool 懒启动，未 start() 过就是零线程）。
    //
    // 清理上次异常退出可能残留的 socket / 命名管道
    QLocalServer::removeServer(name);
    return m_server.listen(name);
}

void RpcServer::setThreadCount(int count)
{
    m_threadCount = count > 0 ? count : 1;

    // 运行中调整也同步到池；Inline 策略下不动池
    if (m_policy == ExecPolicy::ThreadPool)
        m_pool.setMaxThreadCount(m_threadCount);
}

void RpcServer::onNewConnection()
{
    while (QLocalSocket* socket = m_server.nextPendingConnection()) {
        m_buffers[socket] = QByteArray{};

        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { onSocketReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this,
                [this, socket] {
                    dropSubscriptions(socket);   // 断开即退订，避免向死连接推送
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
    }
}

void RpcServer::onSocketReadyRead(QLocalSocket* socket)
{
    // socket 读取、解帧都在服务端线程完成，只把业务调用按策略派发出去
    auto& buffer = m_buffers[socket];
    buffer.append(socket->readAll());

    QByteArray body;
    while (rpc::takeFrame(buffer, body)) {
        switch (rpc::peekType(body)) {
        case rpc::MsgType::Request:
            dispatch(socket, rpc::decodeRequest(body));
            break;
        case rpc::MsgType::Subscribe:
            setSubscription(socket, rpc::decodeTopic(body), true);
            break;
        case rpc::MsgType::Unsubscribe:
            setSubscription(socket, rpc::decodeTopic(body), false);
            break;
        default:
            break;
        }
    }
}

void RpcServer::setSubscription(QLocalSocket* socket, const QString& topic, bool subscribe)
{
    if (subscribe) {
        m_subscribers[topic].insert(socket);
        return;
    }

    const auto it = m_subscribers.find(topic);
    if (it == m_subscribers.end())
        return;

    it.value().remove(socket);
    if (it.value().isEmpty())
        m_subscribers.erase(it);
}

void RpcServer::dropSubscriptions(QLocalSocket* socket)
{
    for (auto it = m_subscribers.begin(); it != m_subscribers.end(); ) {
        it.value().remove(socket);
        if (it.value().isEmpty())
            it = m_subscribers.erase(it);
        else
            ++it;
    }
}

int RpcServer::publishTo(const QString& topic, const QByteArray& frame)
{
    const auto it = m_subscribers.constFind(topic);
    if (it == m_subscribers.cend())
        return 0;   // 没人订阅，直接丢弃

    int sent = 0;
    for (QLocalSocket* socket : it.value()) {
        if (m_buffers.find(socket) == m_buffers.end())
            continue;   // 连接已断开
        socket->write(frame);   // 不 flush：由事件循环合并写出
        ++sent;
    }
    return sent;
}

void RpcServer::dispatch(QLocalSocket* socket, rpc::Request req)
{
    if (m_policy == ExecPolicy::ThreadPool) {
        m_pool.start(new Task(this, QPointer<QLocalSocket>(socket), std::move(req)));
        return;
    }

    // Inline：直接在服务端线程执行
    const QPointer<QLocalSocket> guard(socket);
    const QPointer<RpcServer> self(this);
    const quint64 id = req.id;
    execute(req, [self, guard, id](const QByteArray& payload) {
        if (self)
            self->completeRequest(guard, rpc::Response{id, payload});
    });
}

void RpcServer::execute(const rpc::Request& req,
                        const std::function<void(const QByteArray&)>& reply) const
{
    // reply 可能在任意线程被调用：同步业务在本次调用内完成，
    // 返回 QFuture 的业务在 future 完成后完成。
    // 业务异常不转成回包（异常由业务自己处理），没有回包时客户端按超时处理。
    try {
        m_registry.invoke(req.method, req.args, reply);   // 只读注册表
    } catch (...) {
        // 吞掉异常，不回包
    }
}

void RpcServer::completeRequest(const QPointer<QLocalSocket>& socket, rpc::Response resp)
{
    if (QThread::currentThread() == thread()) {
        finishRequest(socket, resp);
        return;
    }

    QMetaObject::invokeMethod(
        this,
        [this, socket, resp] {
            finishRequest(socket, resp);
        },
        Qt::QueuedConnection);
}

void RpcServer::finishRequest(const QPointer<QLocalSocket>& socket, rpc::Response resp)
{
    writeResponse(socket, rpc::buildResponseFrame(resp));
}

void RpcServer::writeResponse(const QPointer<QLocalSocket>& socket, const QByteArray& frame)
{
    QLocalSocket* s = socket.data();
    if (!s || m_buffers.find(s) == m_buffers.end())
        return;   // 连接已断开，丢弃回包
    s->write(frame);   // 不 flush：由事件循环合并写出
}
