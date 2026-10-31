#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

#include <QByteArray>
#include <QFuture>
#include <QString>

#include "nameof.hpp"
#include "rpc_serialize.h"

// 业务函数可以返回 T 或 QFuture<T>；对客户端而言两者都只关心最终值 T
template <typename T>
struct UnwrapFuture
{
    using type = T;
};

template <typename T>
struct UnwrapFuture<QFuture<T>>
{
    using type = T;
};

template <typename T>
using UnwrapFutureT = typename UnwrapFuture<T>::type;

// nameof 返回编译期 cstring -> QString
template <std::uint16_t N>
inline QString toQString(const nameof::cstring<N>& s)
{
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

// 从函数指针类型取返回类型与参数类型
template <typename F>
struct FunctionTraits;

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)>
{
    using ReturnType = R;
    using ArgsTuple = std::tuple<Args...>;
};

// 从可调用对象（lambda / std::function / 函数指针）推导参数类型，
// 用于「主题是字符串、没有函数符号可参考」时的订阅回调。
// 注意：泛型 lambda（auto 参数）无法推导，需写成具体类型。
template <typename F>
struct CallableTraits : CallableTraits<decltype(&std::decay_t<F>::operator())>
{
};

template <typename R, typename C, typename... Args>
struct CallableTraits<R (C::*)(Args...) const>
{
    using ArgsTuple = std::tuple<Args...>;
};

template <typename R, typename C, typename... Args>
struct CallableTraits<R (C::*)(Args...)>
{
    using ArgsTuple = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct CallableTraits<R (*)(Args...)>
{
    using ArgsTuple = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct CallableTraits<std::function<R(Args...)>>
{
    using ArgsTuple = std::tuple<Args...>;
};

// 按 Tuple 中每个参数（去引用）反序列化 payload，再调用 cb
template <typename Tuple, typename F, std::size_t... I>
void applyWithDeserializeImpl(const QByteArray& payload, F& cb, std::index_sequence<I...>)
{
    auto values = deserialize<std::decay_t<std::tuple_element_t<I, Tuple>>...>(payload);
    std::apply(cb, values);
}

template <typename Tuple, typename F>
void applyWithDeserialize(const QByteArray& payload, F& cb)
{
    applyWithDeserializeImpl<Tuple>(
        payload, cb, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}
