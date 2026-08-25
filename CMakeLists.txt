# CMake 最低版本要求
cmake_minimum_required(VERSION 3.14)

# 项目名称与语言
project(mini_redis_cpp VERSION 0.1.0 LANGUAGES CXX)

# C++ 标准设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # 禁用 GCC/Clang 扩展，保证可移植性

# 编译选项（启用警告，调试符号）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra -Wpedantic)
else()
    add_compile_options(-O2 -DNDEBUG)
endif()

# 查找线程库（虽然单线程，但某些系统需要链接 pthread）
find_package(Threads REQUIRED)

# 可由服务端和测试复用的核心实现
set(CORE_SOURCES
    src/command/Command.cpp
    src/core/Database.cpp
    src/core/Dict.cpp
    src/core/RedisObject.cpp
    src/net/Protocol.cpp
    src/objects/HashObject.cpp
    src/objects/ListObject.cpp
    src/objects/SetObject.cpp
    src/objects/StringObject.cpp
    src/objects/ZSetObject.cpp
    src/objects/intset.cpp
    src/persistence/Rdb.cpp
)

add_library(mini_redis_core STATIC ${CORE_SOURCES})
target_include_directories(mini_redis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mini_redis_core PUBLIC Threads::Threads)

# 创建服务端可执行文件
add_executable(mini_redis_server
    src/main.cpp
    src/net/Connection.cpp
    src/net/Server.cpp
)
target_link_libraries(mini_redis_server PRIVATE mini_redis_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(mini_redis_extended_types_test
        tests/ExtendedTypesTest.cpp
    )
    target_link_libraries(mini_redis_extended_types_test PRIVATE mini_redis_core)
    add_test(NAME extended_types COMMAND mini_redis_extended_types_test)

    add_executable(mini_redis_network_io_test
        tests/NetworkIoTest.cpp
        src/net/Connection.cpp
    )
    target_link_libraries(mini_redis_network_io_test PRIVATE mini_redis_core)
    add_test(NAME network_io COMMAND mini_redis_network_io_test)

    add_executable(mini_redis_protocol_test
        tests/ProtocolTest.cpp
    )
    target_link_libraries(mini_redis_protocol_test PRIVATE mini_redis_core)
    add_test(NAME protocol COMMAND mini_redis_protocol_test)

    add_executable(mini_redis_command_semantics_test
        tests/CommandSemanticsTest.cpp
    )
    target_link_libraries(mini_redis_command_semantics_test PRIVATE mini_redis_core)
    add_test(NAME command_semantics COMMAND mini_redis_command_semantics_test)

    add_executable(mini_redis_hash_encoding_test
        tests/HashEncodingTest.cpp
    )
    target_link_libraries(mini_redis_hash_encoding_test PRIVATE mini_redis_core)
    add_test(NAME hash_encoding COMMAND mini_redis_hash_encoding_test)

    add_executable(mini_redis_rdb_round_trip_test
        tests/RdbRoundTripTest.cpp
    )
    target_link_libraries(mini_redis_rdb_round_trip_test PRIVATE mini_redis_core)
    add_test(NAME rdb_round_trip COMMAND mini_redis_rdb_round_trip_test)

    add_executable(mini_redis_multi_client_io_test
        tests/MultiClientIoTest.cpp
        src/net/Connection.cpp
    )
    target_link_libraries(mini_redis_multi_client_io_test PRIVATE mini_redis_core)
    add_test(NAME multi_client_io COMMAND mini_redis_multi_client_io_test)
endif()

# （可选）设置输出目录
set_target_properties(mini_redis_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 安装规则（可选）
install(TARGETS mini_redis_server
        DESTINATION bin)

