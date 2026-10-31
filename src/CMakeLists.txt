# qrpc 库本体
if(QRPC_BUILD_SHARED_LIBS)
    set(_qrpc_lib_type SHARED)
else()
    set(_qrpc_lib_type STATIC)
endif()

add_library(qrpc ${_qrpc_lib_type}
    "${PROJECT_SOURCE_DIR}/include/qrpc/nameof.hpp"
    "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_serialize.h"
    "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_meta.h"
    "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_protocol.h"
    "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_registry.h"
    "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_server.h"
    "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_client.h"
    rpc_server.cpp
    rpc_client.cpp
)
add_library(qrpc::qrpc ALIAS qrpc)

target_compile_features(qrpc PUBLIC cxx_std_20)

target_include_directories(qrpc PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(qrpc PUBLIC Qt::Core Qt::Network)

# 配合手写的 include/qrpc/qrpc_export.h：
#   构建动态库 -> 库内部 QRPC_BUILD_LIB（Q_DECL_EXPORT），使用方 Q_DECL_IMPORT；
#   构建静态库 -> QRPC_STATIC 同时作用于库与使用方（PUBLIC 定义会随导出 target 传播）。
if(QRPC_BUILD_SHARED_LIBS)
    target_compile_definitions(qrpc PRIVATE QRPC_BUILD_LIB)
else()
    target_compile_definitions(qrpc PUBLIC QRPC_STATIC)
endif()

set_target_properties(qrpc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME qrpc
)

if(QRPC_INSTALL)
    include(GNUInstallDirs)

    set(QRPC_PUBLIC_HEADERS
        "${PROJECT_SOURCE_DIR}/include/qrpc/qrpc_export.h"
        "${PROJECT_SOURCE_DIR}/include/qrpc/nameof.hpp"
        "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_serialize.h"
        "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_meta.h"
        "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_protocol.h"
        "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_registry.h"
        "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_server.h"
        "${PROJECT_SOURCE_DIR}/include/qrpc/rpc_client.h"
    )

    install(TARGETS qrpc EXPORT qrpcTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(FILES ${QRPC_PUBLIC_HEADERS}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/qrpc")

    install(EXPORT qrpcTargets
        FILE qrpcTargets.cmake
        NAMESPACE qrpc::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qrpc")
endif()
