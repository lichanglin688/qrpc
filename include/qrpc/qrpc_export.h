#ifndef QRPC_EXPORT_H
#define QRPC_EXPORT_H

// 符号导出宏，写法参照 Qt 自己的导出头（QtCore/qtcore-config.h 等）：
//   - 构建动态库时，库内部用 Q_DECL_EXPORT，使用方用 Q_DECL_IMPORT；
//   - 构建静态库时（QRPC_STATIC），宏展开为空。
//
// 库自身由 CMake 定义 QRPC_BUILD_LIB；静态构建时 QRPC_STATIC 同时作用于
// 库和使用方（PUBLIC 编译定义会随导出的 target 一起传播）。

#include <QtCore/qglobal.h>

#if defined(QRPC_STATIC)
#  define QRPC_EXPORT
#elif defined(QRPC_BUILD_LIB)
#  define QRPC_EXPORT Q_DECL_EXPORT
#else
#  define QRPC_EXPORT Q_DECL_IMPORT
#endif

#endif // QRPC_EXPORT_H
