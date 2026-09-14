# PlanView — HMI 组态软件（LogicPlanner 设计器 + PageViewer 运行器 + pv_base 共享库）
cmake_minimum_required(VERSION 3.20)

project(PlanView VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 可执行文件统一输出到 out/build/<preset>/ 根目录（不随子目录分散）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# 多配置生成器（如 VS 打开）会追加 <Config>，一并归位，保持目录扁平
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} ${CMAKE_BINARY_DIR})
endforeach()

if(MSVC)
    # 源码含中文注释/字符串，/utf-8 必需；/MP 并行编译
    add_compile_options(/utf-8 /MP)
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(src/base)
add_subdirectory(src/planner)
add_subdirectory(src/viewer)

enable_testing()
add_subdirectory(tests)
