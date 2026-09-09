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
    src/cluster/ClusterConfig.cpp
    src/cluster/MetadataCommand.cpp
    src/cluster/MetadataController.cpp
    src/cluster/MetadataStateMachine.cpp
    src/cluster/MetadataWatcher.cpp
    src/cluster/MigrationAuth.cpp
    src/cluster/Router.cpp
    src/cluster/Slot.cpp
    src/cluster/SlotMigration.cpp
    src/cluster/SlotOwnership.cpp
    src/cluster/Topology.cpp
    src/command/CanonicalCommand.cpp
    src/command/Command.cpp
    src/command/CommandSpec.cpp
    src/consensus/MemoryStorage.cpp
    src/consensus/MultiRaft.cpp
    src/consensus/RaftCore.cpp
    src/consensus/RaftLog.cpp
    src/consensus/RaftTypes.cpp
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

    add_executable(mini_redis_expiration_test
        tests/ExpirationTest.cpp
    )
    target_link_libraries(mini_redis_expiration_test PRIVATE mini_redis_core)
    add_test(NAME expiration COMMAND mini_redis_expiration_test)

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

    add_executable(mini_redis_rdb_compatibility_test
        tests/RdbCompatibilityTest.cpp
    )
    target_link_libraries(mini_redis_rdb_compatibility_test PRIVATE mini_redis_core)
    add_test(NAME rdb_compatibility COMMAND mini_redis_rdb_compatibility_test)

    add_executable(mini_redis_multi_client_io_test
        tests/MultiClientIoTest.cpp
        src/net/Connection.cpp
    )
    target_link_libraries(mini_redis_multi_client_io_test PRIVATE mini_redis_core)
    add_test(NAME multi_client_io COMMAND mini_redis_multi_client_io_test)

    add_executable(mini_redis_cluster_slot_test
        tests/ClusterSlotTest.cpp
    )
    target_link_libraries(mini_redis_cluster_slot_test PRIVATE mini_redis_core)
    add_test(NAME cluster_slot COMMAND mini_redis_cluster_slot_test)

    add_executable(mini_redis_cluster_routing_test
        tests/ClusterRoutingTest.cpp
    )
    target_link_libraries(mini_redis_cluster_routing_test PRIVATE mini_redis_core)
    add_test(NAME cluster_routing COMMAND mini_redis_cluster_routing_test)

    add_executable(mini_redis_raft_core_test
        tests/RaftCoreTest.cpp
    )
    target_link_libraries(mini_redis_raft_core_test PRIVATE mini_redis_core)
    add_test(NAME raft_core COMMAND mini_redis_raft_core_test)

    add_executable(mini_redis_deterministic_state_machine_test
        tests/DeterministicStateMachineTest.cpp
    )
    target_link_libraries(mini_redis_deterministic_state_machine_test PRIVATE mini_redis_core)
    add_test(NAME deterministic_state_machine COMMAND mini_redis_deterministic_state_machine_test)

    add_executable(mini_redis_metadata_state_machine_test
        tests/MetadataStateMachineTest.cpp
    )
    target_link_libraries(mini_redis_metadata_state_machine_test PRIVATE mini_redis_core)
    add_test(NAME metadata_state_machine COMMAND mini_redis_metadata_state_machine_test)

    add_executable(mini_redis_metadata_raft_test
        tests/MetadataRaftTest.cpp
    )
    target_link_libraries(mini_redis_metadata_raft_test PRIVATE mini_redis_core)
    add_test(NAME metadata_raft COMMAND mini_redis_metadata_raft_test)

    add_executable(mini_redis_slot_migration_test
        tests/SlotMigrationTest.cpp
    )
    target_link_libraries(mini_redis_slot_migration_test PRIVATE mini_redis_core)
    add_test(NAME slot_migration COMMAND mini_redis_slot_migration_test)
endif()

# （可选）设置输出目录
set_target_properties(mini_redis_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 安装规则（可选）
install(TARGETS mini_redis_server
        DESTINATION bin)

