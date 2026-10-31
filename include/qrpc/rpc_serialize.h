#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QDataStream>
#include <stdexcept>
#include <tuple>

// 通用序列化：把任意可被 QDataStream << 的参数打包成一个 QByteArray
template<typename... Args>
QByteArray serialize(const Args&... args)
{
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_DefaultCompiledVersion);

    ((out << args), ...);

    if (out.status() != QDataStream::Ok)
        throw std::runtime_error("QDataStream serialize failed");

    return data;
}

// 反序列化：按给定类型从 QByteArray 取出 tuple
template<typename... Args>
std::tuple<Args...> deserialize(const QByteArray& data)
{
    QByteArray bytes = data;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open input buffer");

    QDataStream in(&buffer);
    in.setVersion(QDataStream::Qt_DefaultCompiledVersion);

    std::tuple<Args...> result{};
    std::apply(
        [&in](auto&... args) {
            ((in >> args), ...);
        },
        result
    );

    if (in.status() != QDataStream::Ok)
        throw std::runtime_error("QDataStream deserialize failed");

    if (!buffer.atEnd())
        throw std::runtime_error("Unexpected trailing data");

    return result;
}
