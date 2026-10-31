# 两个示例，区别只在服务端的执行策略，都直接运行、不带命令行参数：
#   qrpc_demo_inline —— ExecPolicy::Inline，业务在服务端线程执行，
#                        包含返回 QFuture 的异步业务、超时与发布订阅；
#   qrpc_demo_pool   —— ExecPolicy::ThreadPool，业务投递到自有线程池并发执行，
#                        只使用同步业务函数。
qt_add_executable(qrpc_demo_inline inline/main.cpp)
target_link_libraries(qrpc_demo_inline PRIVATE qrpc::qrpc)

qt_add_executable(qrpc_demo_pool pool/main.cpp)
target_link_libraries(qrpc_demo_pool PRIVATE qrpc::qrpc)
