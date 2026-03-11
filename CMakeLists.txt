# =============================================================================
# CMake 最低版本 + 项目定义
# =============================================================================
cmake_minimum_required(VERSION 3.10)
project(cometIOCP VERSION 1.0.0 LANGUAGES CXX)

# =============================================================================
# 库目标定义
# =============================================================================
add_library(${PROJECT_NAME}
    src/driver.cpp
    src/driver.h
    src/types.h
    src/util.h
    include/cometIOCP.h
)

# =============================================================================
# Target-specific 设置（不影响父项目）
# =============================================================================

# C++ 标准
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

# MSVC 编译选项
if(MSVC)
    target_compile_options(${PROJECT_NAME} PUBLIC /utf-8)
endif()

# 编译定义
target_compile_definitions(${PROJECT_NAME}
    PUBLIC
        WIN32_LEAN_AND_MEAN      # 减少 Windows.h 内容，加快编译
        _WIN32_WINNT=0x0601      # 最低支持 Windows 7
        UNICODE
        _UNICODE
)

# 头文件搜索路径
target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# 链接库
target_link_libraries(${PROJECT_NAME}
    PUBLIC
        ws2_32
        mswsock
)

# =============================================================================
# 示例程序
# =============================================================================
add_executable(echo_server example/server.cpp)
target_link_libraries(echo_server PRIVATE ${PROJECT_NAME})

add_executable(echo_client example/client.cpp)
target_link_libraries(echo_client PRIVATE ${PROJECT_NAME})

# =============================================================================
# 测试程序
# =============================================================================
add_executable(stress_test test/stress_test.cpp)
target_link_libraries(stress_test PRIVATE ${PROJECT_NAME})