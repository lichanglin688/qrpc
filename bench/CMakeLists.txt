# 性能压测：串行 / 流水线 / 多连接三种场景
# 用法：qrpc_bench --policy inline --serial 2000 --conc 20000 [--window W] [--clients K]
qt_add_executable(qrpc_bench rpc_bench.cpp)
target_link_libraries(qrpc_bench PRIVATE qrpc::qrpc)
