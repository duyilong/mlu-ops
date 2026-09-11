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
#ifndef CORE_LOG_CORE_LOG_CALLBACK_SINK_H_
#define CORE_LOG_CORE_LOG_CALLBACK_SINK_H_

#include <atomic>
#include <mutex>

#include "spdlog/sinks/base_sink.h"

namespace mluop {
namespace log {

// 用户回调签名：`severity` 为新引擎 Severity 的整数值，`function_name` 为当前正在
// 执行的公共 API 名（**暂固定传空指针**，栈回溯解析方案暂缓，见 §12.17；保留该参数
// 以对齐 CUDA 系回调语义、便于日后按 API 维度过滤），`message` 为已格式化（含
// pattern 前缀）的完整日志串。与 C API 的 MluOpLogCallback 布局一致（均为
// void(int,const char*,const char*) 函数指针），可安全 reinterpret_cast 互转。
// （不携带 user_data 透传参数：两个独立 atomic 的成对撕裂难以彻底消除、且当前无
// 调用方需要，2026-09-09 决策删除，见动作日志 §12.20。）
using LogCallback = void (*)(int severity, const char* function_name,
                             const char* message);

// 回调 sink：把 spdlog 输出的每条日志转发给用户回调（经 LogEngine::setLogCallback
// 接线到 logger_ 的 sinks_init_list）。
//
// 线程安全：callback_ 为 std::atomic（relaxed load/store）。写侧 set_callback/clear
// 与读侧 sink_it_ 可能跨线程并发（setLogCallback 在任意线程、日志发射在任意线程），
// atomic 避免数据竞争 UB。不能改用 mutex_ 保护：用户回调在 sink_it_（持
// base_sink::mutex_）内被调用，回调内再 set_callback 去锁同一非递归 mutex_ 会死锁，
// 故写侧刻意不取锁、改用原子。
template <typename Mutex>
class LogCallbackSink : public spdlog::sinks::base_sink<Mutex> {
 public:
  explicit LogCallbackSink(LogCallback callback = nullptr)
      : callback_(callback) {}

  void set_callback(LogCallback callback) {
    callback_.store(callback, std::memory_order_relaxed);
  }

  void clear() { callback_.store(nullptr, std::memory_order_relaxed); }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    LogCallback callback = callback_.load(std::memory_order_relaxed);
    if (callback == nullptr) {
      return;
    }
    // functionName 暂固定传空指针（栈回溯解析暂缓，见 §12.17）。
    callback(static_cast<int>(msg.level), /*function_name=*/nullptr,
             msg.payload.data());
  }

  void flush_() override {}

  std::atomic<LogCallback> callback_;
};

using LogCallbackSinkMt = LogCallbackSink<std::mutex>;

}  // namespace log
}  // namespace mluop

#endif  // CORE_LOG_CORE_LOG_CALLBACK_SINK_H_
