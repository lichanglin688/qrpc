#pragma once

#include <QBasicTimer>
#include <QByteArray>
#include <QLocalSocket>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "qrpc_export.h"   // QRPC_EXPORT
#include "rpc_meta.h"
#include "rpc_protocol.h"
#include "rpc_serialize.h"

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4251)   // std 容器成员不需要 dll-interface
#endif

// 调用结果只有两种：ok=true 看 value；ok=false 看 error（目前只会是 "timeout"）。
template <typename R>
struct RpcResult
{
    bool ok = false;
    R value{};
    QString error;
};

template <>
struct RpcResult<void>
{
    bool ok = false;
    QString error;
};

namespace rpc_client_detail
{
// 把参数元组的前 N 项打包成 payload（末项是回调，不参与序列化）
template <typename Tuple, std::size_t... I>
QByteArray serializeArgs(Tuple&& values, std::index_sequence<I...>)
{
    return serialize(std::get<I>(std::forward<Tuple>(values))...);
}
} // namespace rpc_client_detail

// 基于 QLocalSocket 的异步 RPC 客户端。
//  - callAsync<fn>(args..., cb)：返回 void，结果通过 cb(const RpcResult<R>&) 给出；
//  - setTimeout(ms)：超时时回 ok=false、error == "timeout"；
//  - onPush<fn>(cb)：订阅服务端推送；
//  - 想用 QFuture 的用户在回调之上自己包一层即可（见 examples/inline/main.cpp 的 callAsFuture）。
// 资源释放全部在析构函数中显式完成，不依赖成员声明顺序。
class QRPC_EXPORT RpcClient : public QObject
{
    Q_OBJECT
public:
    explicit RpcClient(QObject* parent = nullptr);
    ~RpcClient() override;

    void connectToServer(const QString& name);

    void setTimeout(int ms) { m_timeoutMs = ms; }

    // 异步调用：参数在前，回调固定是最末一个实参；
    // 回调只有一种形态 void(const RpcResult<R>&)，R 由函数返回类型推导
    template<auto Function, typename... Args>
    void callAsync(Args&&... args)
    {
        // 服务端可能返回 QFuture<T>，对客户端来说结果类型就是 T
        using R = UnwrapFutureT<typename FunctionTraits<decltype(Function)>::ReturnType>;

        constexpr std::size_t N = sizeof...(Args);
        static_assert(N >= 1, "callAsync 的最后一个实参必须是回调");
        using ArgTuple = std::tuple<Args&&...>;
        using Cb = std::decay_t<std::tuple_element_t<N - 1, ArgTuple>>;
        static_assert(std::is_invocable_v<Cb&, const RpcResult<R>&>,
                      "回调签名必须是 void(const RpcResult<R>&)");

        auto argTuple = std::forward_as_tuple(std::forward<Args>(args)...);
        Cb cb(std::get<N - 1>(argTuple));

        const quint64 id = m_nextId++;
        const QString method = toQString(::nameof::nameof_pointer<Function>());
        const QByteArray argsPayload =
            rpc_client_detail::serializeArgs(argTuple, std::make_index_sequence<N - 1>{});

        Pending& pending = m_pending[id];
        pending.handler =
            [cb = std::move(cb)](bool ok, const QByteArray& payload) mutable
            {
                RpcResult<R> result;
                result.ok = ok;
                if (!ok) {
                    result.error = QString::fromUtf8(payload);
                } else if constexpr (!std::is_void_v<R>) {
                    result.value = std::get<0>(deserialize<std::decay_t<R>>(payload));
                }
                cb(result);
            };

        // 每个请求一个轻量定时器：收到响应时可以立刻 stop，不必空转到超时才销毁。
        // QBasicTimer 只有 16 字节、无堆分配，超时统一由 timerEvent 处理。
        if (m_timeoutMs > 0) {
            pending.timer.start(m_timeoutMs, this);
            m_timerIndex.emplace(pending.timer.timerId(), id);
        }

        sendRequest(id, method, argsPayload);
    }

    // 一、字符串主题：主题是普通字符串，回调参数类型由回调签名推导（支持运行时拼主题）。
    // 注册处理函数的同时向服务端订阅；未连接时先缓存，连上（或重连）后自动重发订阅。
    template<typename Callback>
    void subscribe(const QString& topic, Callback&& cb)
    {
        using ArgsTuple = typename CallableTraits<std::decay_t<Callback>>::ArgsTuple;
        subscribeAs<ArgsTuple>(topic, std::forward<Callback>(cb));
    }

    // 二、函数符号：主题名由 nameof 推导，回调参数类型来自函数签名，
    // 与服务端 publish<fn>() 对称；强类型、无拼写风险，泛型 lambda 也能用。
    template<auto Function, typename Callback>
    void subscribe(Callback&& cb)
    {
        using ArgsTuple = typename FunctionTraits<decltype(Function)>::ArgsTuple;
        subscribeAs<ArgsTuple>(toQString(::nameof::nameof_pointer<Function>()),
                              std::forward<Callback>(cb));
    }

    // 取消订阅并移除处理函数
    void unsubscribe(const QString& topic)
    {
        m_subscriptions.erase(topic.toStdString());
        sendTopic(rpc::MsgType::Unsubscribe, topic);
    }

    template<auto Function>
    void unsubscribe()
    {
        unsubscribe(toQString(::nameof::nameof_pointer<Function>()));
    }

signals:
    void connected();
    void disconnected();
    void socketError(const QString& message);
    void pushUnhandled(const QString& name);

protected:
    // QBasicTimer 的超时事件：按 timerId 找回请求并判定超时
    void timerEvent(QTimerEvent* event) override;

private:
    // 两种订阅入口共用：按已推导出的参数类型挂处理函数并发订阅
    template<typename ArgsTuple, typename Callback>
    void subscribeAs(const QString& topic, Callback&& cb)
    {
        m_subscriptions[topic.toStdString()] =
            [cb = std::forward<Callback>(cb)](const QByteArray& payload) mutable {
                applyWithDeserialize<ArgsTuple>(payload, cb);
            };

        sendTopic(rpc::MsgType::Subscribe, topic);
    }

    void sendTopic(rpc::MsgType type, const QString& name);
    void onReadyRead();
    void sendRequest(quint64 id, const QString& method, const QByteArray& args);
    void handleFrame(const QByteArray& body);
    void handleResponse(const QByteArray& body);
    void handlePush(const QByteArray& body);

    QLocalSocket m_socket;
    QByteArray m_buffer;
    quint64 m_nextId = 1;
    int m_timeoutMs = 5000;

    // 一个在途请求：(ok, payload) 完成回调 + 它自己的超时定时器（可随时 stop）
    struct Pending
    {
        std::function<void(bool, const QByteArray&)> handler;
        QBasicTimer timer;
    };

    std::unordered_map<quint64, Pending> m_pending;
    std::unordered_map<int, quint64> m_timerIndex;   // timerId -> 请求 id
    std::unordered_map<std::string, std::function<void(const QByteArray&)>> m_subscriptions;
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
