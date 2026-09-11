# -----------------------------------------------------------------------------
# 新日志引擎（spdlog 后端）构建配置。
#
# 宏层已恒走新 spdlog 引擎（无新旧双轨开关），本文件只负责 spdlog/fmt 依赖接线。
# -----------------------------------------------------------------------------

# 核心库只链接 fmt 的 header-only 目标 fmt::fmt-header-only，因此 spdlog 必须走
# SPDLOG_FMT_EXTERNAL_HO（与 SPDLOG_FMT_EXTERNAL 互斥；详见方案书 §15 / §9）。
set(SPDLOG_FMT_EXTERNAL_HO ON CACHE BOOL "Use external fmt header-only" FORCE)

# 只构建静态库；关闭示例 / 测试 / 基准 / 安装，避免引入多余目标。
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

# 幂等：根 / open_mlu_ops 两套 CMakeLists 均会 include 本文件，避免重复
# add_subdirectory。spdlog 自带 fmt 探测（find_package(fmt...)）由 SPDLOG_FMT_EXTERNAL_HO
# 触发，且要求 fmt::fmt-header-only 已存在——本文件必须在 find_package(fmt) 之后 include。
if(NOT TARGET spdlog::spdlog)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../third_party/spdlog
                   ${CMAKE_BINARY_DIR}/spdlog)
endif()
