#pragma once

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QString>

#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "qrpc_export.h"
#include "rpc_meta.h"
#include "rpc_serialize.h"

namespace rpc_detail
{
template <typename T>
struct IsQFuture : std::false_type {};

template <typename T>
struct IsQFuture<QFuture<T>> : std::true_type {};

template <typename T>
inline constexpr bool isQFutureV = IsQFuture<T>::value;

// 把业务返回的 QFuture 接到完成回调上：正常完成时序列化结果并回调。
// future 失败时不回包（客户端按超时处理），异常不会逃到线程池里 terminate。
// 回调触发的线程不确定，调用方负责切回自己的线程。
template <typename T, typename Done>
void attachFuture(QFuture<T> future, Done done)
{
    if constexpr (std::is_void_v<T>)
        future.then([done] { done(QByteArray{}); });
    else
        future.then([done](const T& value) { done(serialize(value)); });
}
} // namespace rpc_detail

// 服务端注册表：按名字注册函数，收到 (name, payload) 后
// 反序列化参数 -> 调用 -> 序列化返回值。
//
// 支持两种业务函数形态：
//   R fn(Args...)            —— 同步：本次调用内直接给出结果；
//   QFuture<R> fn(Args...)   —— 异步：future 完成后才回包，期间执行线程不被占用。
class QRPC_EXPORT RpcRegistry
{
public:
    using Completion = std::function<void(const QByteArray& payload)>;

    // 无宏自动取名：函数作为模板参数（add 或 &add）
    template<auto Function>
    void registerHandler()
    {
        registerHandler(toQString(::nameof::nameof_pointer<Function>()), Function);
    }

    template<typename R, typename... Args>
    void registerHandler(const QString& name, R (*function)(Args...))
    {
        if (m_handlers.contains(name))
            throw std::runtime_error(
                (QStringLiteral("RPC function already registered: ") + name).toStdString());

        m_handlers.insert(
            name,
            [function](const QByteArray& request, const Completion& done)
            {
                auto args = deserialize<std::decay_t<Args>...>(request);

                if constexpr (std::is_void_v<R>) {
                    std::apply(function, args);
                    done(QByteArray{});
                } else if constexpr (rpc_detail::isQFutureV<R>) {
                    rpc_detail::attachFuture(std::apply(function, args), done);
                } else {
                    done(serialize(std::apply(function, args)));
                }
            });
    }

    // 统一入口：同步函数的 done 在本次调用内被调用；返回值是 QFuture 的函数
    // 会在 future 完成后调用 done，且可能来自任意线程。
    void invoke(const QString& name, const QByteArray& request, Completion done) const
    {
        const auto it = m_handlers.constFind(name);

        if (it == m_handlers.cend())
            throw std::runtime_error(
                (QStringLiteral("RPC function not found: ") + name).toStdString());

        it.value()(request, done);
    }

private:
    QHash<QString, std::function<void(const QByteArray&, const Completion&)>> m_handlers;
};
