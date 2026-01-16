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

# 源文件列表（按模块组织）
set(SOURCES
    main.cpp
    Server.cpp
    Connection.cpp
    Protocol.cpp
    Database.cpp
    Command.cpp
    RedisObject.cpp
    Rdb.cpp
    # 如果后续添加 Config.cpp，也加在这里
)

# 创建可执行文件
add_executable(mini_redis_server ${SOURCES})

# 链接系统线程库（Linux/macOS 需要）
target_link_libraries(mini_redis_server PRIVATE Threads::Threads)

# （可选）设置输出目录
set_target_properties(mini_redis_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 安装规则（可选）
install(TARGETS mini_redis_server
        DESTINATION bin)

