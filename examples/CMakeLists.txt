# 示例：单进程里同时起 RpcServer 和 RpcClient，演示调用、超时、
# 服务端异步业务（QFuture）与发布订阅。
# 用法：qrpc_demo [inline|pool]
qt_add_executable(qrpc_demo demo/main.cpp)
target_link_libraries(qrpc_demo PRIVATE qrpc::qrpc)
