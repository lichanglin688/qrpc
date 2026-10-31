#pragma once

#include <QObject>
#include <QLocalServer>
#include <QPointer>
#include <QThreadPool>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "qrpc_export.h"
#include "rpc_protocol.h"
#include "rpc_registry.h"

class QLocalSocket;

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4251)   // std 容器成员不需要 dll-interface
#endif

// 基于 QLocalServer 的 RPC 服务端。
//
// 两种业务执行策略（setExecPolicy）：
//   Inline     —— 直接在服务端线程执行，零跨线程切换，最轻；
//   ThreadPool —— 投递到自有 QThreadPool，每请求一个任务，并发执行。
//
// 无论哪种策略：socket 读写与帧解析都在服务端线程，池线程只跑业务函数，
// 回包通过队列调用切回服务端线程。资源释放全部在析构函数中显式完成。
class QRPC_EXPORT RpcServer : public QObject
{
    Q_OBJECT
public:
    enum class ExecPolicy
    {
        Inline,
        ThreadPool
    };

    explicit RpcServer(QObject* parent = nullptr);
    ~RpcServer() override;

    bool listen(const QString& name);

    // 只有切到 ThreadPool 时才配置自有线程池；
    // QThreadPool 是懒启动的，没有被 start() 过就不会创建线程，Inline 下完全不碰它。
    void setExecPolicy(ExecPolicy policy)
    {
        m_policy = policy;
        if (m_policy == ExecPolicy::ThreadPool)
            m_pool.setMaxThreadCount(m_threadCount > 0 ? m_threadCount : 1);
    }
    ExecPolicy execPolicy() const { return m_policy; }

    // 线程池线程数；仅在 ThreadPool 策略下影响池
    void setThreadCount(int count);
    int threadCount() const { return m_threadCount; }

    // 注意：注册必须在 listen 之前完成，之后注册表只读，池线程并发读取安全
    template<auto Function>
    void registerHandler()
    {
        m_registry.registerHandler<Function>();
    }

    // 一、字符串主题：只发给订阅了该主题的客户端，返回实际发送到的客户端数量。
    // 主题可以是运行时拼出来的字符串，参数类型由实参推导。
    template<typename... Args>
    int publish(const QString& topic, Args&&... args)
    {
        rpc::Push p;
        p.name = topic;
        p.payload = serialize(args...);

        return publishTo(topic, rpc::buildPushFrame(p));
    }

    // 二、函数符号：主题名由 nameof 推导，与客户端 subscribe<fn>() 对称，
    // 无字符串拼写风险（同一代码库内优先用这个）。
    template<auto Function, typename... Args>
    int publish(Args&&... args)
    {
        return publish(toQString(::nameof::nameof_pointer<Function>()),
                       std::forward<Args>(args)...);
    }

private:
    class Task;   // ThreadPool 策略用的 QRunnable

    void onNewConnection();
    void onSocketReadyRead(QLocalSocket* socket);
    void dispatch(QLocalSocket* socket, rpc::Request req);

    // 调用业务函数并回包：同步函数在本次调用内完成，返回 QFuture 的函数稍后完成
    void execute(const rpc::Request& req,
                 const std::function<void(const QByteArray&)>& reply) const;
    // 任意线程可调用：内部切回服务端线程写 socket
    void completeRequest(const QPointer<QLocalSocket>& socket, rpc::Response resp);
    void finishRequest(const QPointer<QLocalSocket>& socket, rpc::Response resp);
    void writeResponse(const QPointer<QLocalSocket>& socket, const QByteArray& frame);

    // 主题订阅表：topic -> 订阅者集合，只在服务端线程访问
    void setSubscription(QLocalSocket* socket, const QString& topic, bool subscribe);
    void dropSubscriptions(QLocalSocket* socket);
    int publishTo(const QString& topic, const QByteArray& frame);

    QLocalServer m_server;
    QThreadPool m_pool;
    RpcRegistry m_registry;

    ExecPolicy m_policy = ExecPolicy::Inline;
    int m_threadCount = 1;

    std::unordered_map<QLocalSocket*, QByteArray> m_buffers;   // 只在服务端线程访问
    std::unordered_map<std::string, std::unordered_set<QLocalSocket*>> m_subscribers;
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
