# 单元测试：每个源文件一个可执行 + 一条 ctest 用例
find_package(Qt6 REQUIRED COMPONENTS Test)

set(QRPC_TESTS
    test_protocol.cpp        # 帧协议的编解码与粘包/半包处理
    test_serialization.cpp   # QDataStream 打包 unpack
    test_registry.cpp        # 注册表：同步函数与 QFuture 异步函数
    test_client_server.cpp   # 端到端：调用、返回值、超时、发布订阅
)

foreach(_source IN LISTS QRPC_TESTS)
    get_filename_component(_name "${_source}" NAME_WE)

    qt_add_executable(${_name} ${_source})
    target_link_libraries(${_name} PRIVATE qrpc::qrpc Qt6::Test)

    add_test(NAME ${_name} COMMAND ${_name})
endforeach()
