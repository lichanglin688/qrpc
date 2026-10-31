cmake_minimum_required(VERSION 3.16)

project(qrpc
    VERSION 0.1.0
    DESCRIPTION "Header-friendly Qt6 RPC library over QLocalSocket"
    LANGUAGES CXX)

# C++20：nameof + 模板参数是这套强类型 API 的基础
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(QRPC_BUILD_SHARED_LIBS "构建动态库（关闭则退化为静态库）" ON)
option(QRPC_BUILD_EXAMPLES    "构建示例程序"                     ON)
option(QRPC_BUILD_BENCHMARK   "构建性能压测程序"                 ON)
option(QRPC_BUILD_TESTS       "构建单元测试"                     ON)
option(QRPC_INSTALL           "生成安装与导出规则"               ON)

# Core：基础；Network：QLocalSocket / QLocalServer
find_package(Qt6 REQUIRED COMPONENTS Core Network)

# Qt6 便捷接口：自动开启 AUTOMOC/AUTORCC/AUTOUIC 并设置合适的 C++ 标准
qt_standard_project_setup()

# 统一输出目录：动态库与可执行文件放在一起，Windows 下运行不必手工拷贝 dll
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_subdirectory(src)

if(QRPC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(QRPC_BUILD_BENCHMARK)
    add_subdirectory(bench)
endif()

if(QRPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(QRPC_INSTALL)
    # 导出目标本身由 src/CMakeLists.txt 完成，这里补齐包配置，
    # 让使用方能直接 find_package(qrpc)。
    include(CMakePackageConfigHelpers)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qrpcConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/qrpcConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qrpc")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/qrpcConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/qrpcConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/qrpcConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qrpc")
endif()
