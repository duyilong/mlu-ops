/*************************************************************************
 * Copyright (C) [2022] by Cambricon, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *************************************************************************/
#ifndef CORE_LOG_CORE_LOG_PROJECT_H_
#define CORE_LOG_CORE_LOG_PROJECT_H_

namespace mluop {
namespace logging {

// 项目级日志配置常量：集中管理 cnlog 依赖的环境变量名、模块名与展示名，
// 避免散落在实现文件里的字符串字面量。log_core.h / log_core.cpp 统一引用此处。

// 环境变量：允许打印的最高日志级别（取值 0..7 或 ERROR/WARNING/API_TRACE/...）。
inline constexpr const char* kEnvMaxLogLevel = "CNNL_MAX_LOG_LEVEL";
// 环境变量：是否只输出到屏幕（不落盘）。
inline constexpr const char* kEnvOnlyShow = "MLUOP_LOG_ONLY_SHOW";
// 环境变量：是否彩色打印。
inline constexpr const char* kEnvColorPrint = "MLUOP_LOG_COLOR_PRINT";
// 环境变量：是否打印指定模块。
inline constexpr const char* kEnvModulePrint = "MLUOP_LOG_PRINT";

// 模块名（module_print_map_ 的键，也是 CLOG 宏传入的 #module）。
inline constexpr const char* kProjectModuleName = "MLUOP";
// 日志头展示名。
inline constexpr const char* kProjectDisplayName = "MLU-OPS";
// 日志落盘文件名（非 only_show 模式下）。
inline constexpr const char* kLogFileName = "mlu_op_auto_log";

}  // namespace logging
}  // namespace mluop

#endif  // CORE_LOG_CORE_LOG_PROJECT_H_
