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
#include "core/log_core/log_engine.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include "core/tool.h"
#include "mlu_op.h"

#if defined(WINDOWS) || defined(WIN32)  // used in windows
#include <windows.h>
#include <process.h>
#include <processthreadsapi.h>
#include <io.h>
#else
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <fmt/chrono.h>
#include <fmt/color.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

namespace mluop {
namespace log {

namespace {

// 引擎存活标志（对照旧引擎 cnlogSingletonInitFlag_）：默认=存活（1），仅 ~LogEngine
// 置 0（已析构）。文件级静态原子，进程退出前可安全读取；LogMessage 析构据此在引擎
// 已销毁时跳过发射（不触碰 instance()），避免退出期后台线程/全局析构打日志的
// use-after-free。默认=存活使首条日志经 instance() 按需构造引擎，开箱即有日志。
constexpr int kLogEngineAliveMagic = 1;
std::atomic<int> g_log_engine_alive_flag = kLogEngineAliveMagic;

// 读环境变量并解析为最高可打印级别；未设置或非法回落 default_para。
int maxLogLevelFromEnv(const char* env_name, int default_para) {
  const char* raw = std::getenv(env_name);
  if (raw == nullptr) {
    return default_para;
  }
  return LogEngine::parseMaxLogLevelValue(std::string(raw), default_para);
}

// 是否打印到屏幕：恒写 std::cout，故只判 stdout 是否为 tty。
bool isPrintToScreen() { return isatty(fileno(stdout)); }

// 彩色/复位转义：屏幕 tail 着色用，局部定义。
#if !defined(WIN32) && !defined(COLOR_SILENCE)
constexpr const char* kGreen = "\033[32m";
constexpr const char* kReset = "\033[0m";
#else
constexpr const char* kGreen = "";
constexpr const char* kReset = "";
#endif

// 按 Severity 返回终端颜色（与屏幕 head 着色一致）。
inline static auto formatSeverityColor(Severity severity) {
  switch (severity) {
    case Severity::kError: {
      return fmt::fg(fmt::terminal_color::red);
    }
    case Severity::kWarning: {
      return fmt::fg(fmt::terminal_color::magenta);
    }
    case Severity::kInfo: {
      return fmt::fg(fmt::terminal_color::green);
    }
    case Severity::kCnpapi: {
      return fmt::fg(fmt::terminal_color::blue);
    }
    case Severity::kDebug1:
    case Severity::kDebug2:
    case Severity::kDebug3:
    case Severity::kDebug4: {
      return fmt::fg(fmt::terminal_color::cyan);
    }
  }
  // all enum used
  return fmt::fg(fmt::terminal_color::white);
}

// 按 Severity 返回级别名。
inline static std::string formatSeverityName(Severity severity) {
  switch (severity) {
    case Severity::kError: {
      return "ERROR";
    }
    case Severity::kWarning: {
      return "WARNING";
    }
    case Severity::kInfo: {
      return "INFO";
    }
    case Severity::kCnpapi: {
      return "CNPAPI";
    }
    case Severity::kDebug1: {
      return "DEBUG1";
    }
    case Severity::kDebug2: {
      return "DEBUG2";
    }
    case Severity::kDebug3: {
      return "DEBUG3";
    }
    case Severity::kDebug4: {
      return "DEBUG4";
    }
  }
  return "";
}

// 按 is_colored 决定是否着色加粗。
inline static std::string formatSeverity(Severity severity, bool is_colored) {
  if (is_colored) {
    return fmt::format("{}", fmt::styled(formatSeverityName(severity),
                                         formatSeverityColor(severity) |
                                             fmt::emphasis::bold));
  } else {
    return formatSeverityName(severity);
  }
}

// 获取当前进程 id。
inline static auto getpid_() {
#if defined(WINDOWS) || defined(WIN32)
  return _getpid();
#else
  return getpid();
#endif
}

// 获取当前时间戳：[{F %T}] 精确到微秒。
std::string getTime() {
  auto now = std::chrono::system_clock::now();
  return fmt::format("[{:%F %T}]",
                     std::chrono::time_point_cast<std::chrono::microseconds>(now));
}

// 把消息中的换行替换为空格，保证单行输出。
void clearEnter(std::string* ss) {
  for (std::string::iterator it = (*ss).begin(); it != (*ss).end(); it++) {
    if (*it == '\n') {
      *it = ' ';
    }
  }
}

// 屏幕 sink：逐字节写 msg.payload 到 std::cout（endl = '\n' + flush）。payload
// 非持有，sink 同步链保证在本次 log() 调用内写完。
class ConsoleSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::cout.write(msg.payload.data(),
                    static_cast<std::streamsize>(msg.payload.size()));
    std::cout.put('\n');
    std::cout.flush();
  }
  void flush_() override { std::cout.flush(); }
};

// 文件 sink：逐字节写 msg.payload 到 log_file_stream_（endl = '\n' + flush）。
// 以引用持有流，is_open() 守卫防止流未打开 / 已关闭时写出错。
class FileSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit FileSink(std::ofstream& stream) : stream_(stream) {}

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (stream_.is_open()) {
      stream_.write(msg.payload.data(),
                    static_cast<std::streamsize>(msg.payload.size()));
      stream_.put('\n');
      stream_.flush();
    }
  }
  void flush_() override {
    if (stream_.is_open()) {
      stream_.flush();
    }
  }

 private:
  std::ofstream& stream_;
};

}  // namespace

LogEngine& LogEngine::instance() {
  static LogEngine engine;
  return engine;
}

LogEngine::LogEngine()
    : max_log_level_(maxLogLevelFromEnv(kEnvMaxLogLevel, 0)),
      only_show_(mluop::getBoolEnvVar(kEnvOnlyShow, true)),
      color_print_(mluop::getBoolEnvVar(kEnvColorPrint, true)),
      log_file_(kLogFileName) {
  // 仅当非 only_show 时打开日志文件；打开失败打印提示。随后按 stdout 是否
  // tty 决定是否保留彩色。构造期为单线程初始化，直接读 atomic 值即可。
  if (!only_show_.load(std::memory_order_relaxed)) {
    log_file_stream_.open(log_file_);
    if (!log_file_stream_.is_open()) {
      std::cerr << "can't init Log, open file failed!" << std::endl;
    }
  }
  if (color_print_.load(std::memory_order_relaxed)) {
    color_print_.store(isPrintToScreen(), std::memory_order_relaxed);
  }

  // 接线 spdlog：logger_（屏幕）挂 ConsoleSink + callback_sink_，file_logger_
  // （文件）挂 FileSink（引用 log_file_stream_）。两 logger 均 set_level(trace)
  // —— spdlog logger 默认 info(2) 会滤掉 ERROR/WARNING（severity 0/1），必须放行。
  // Emit 里已按 max_log_level_ 在函数开头门控，此处只需不额外过滤。
  callback_sink_ = std::make_shared<LogCallbackSinkMt>();
  logger_ = std::make_shared<spdlog::logger>(
      kProjectDisplayName,
      spdlog::sinks_init_list{std::make_shared<ConsoleSink>(), callback_sink_});
  logger_->set_level(spdlog::level::trace);

  file_logger_ = std::make_shared<spdlog::logger>(
      kProjectDisplayName, std::make_shared<FileSink>(log_file_stream_));
  file_logger_->set_level(spdlog::level::trace);

  // 构造完成，置存活标志，允许日志发射。
  g_log_engine_alive_flag.store(kLogEngineAliveMagic, std::memory_order_relaxed);
}

LogEngine::~LogEngine() {
  // 析构开始即清存活标志：此后日志发射在 LogMessage 析构入口被拦下，
  // 不会访问已销毁的成员。
  g_log_engine_alive_flag.store(0, std::memory_order_relaxed);
}

int LogEngine::maxLogLevel() const {
  return max_log_level_.load(std::memory_order_relaxed);
}

void LogEngine::setMaxLogLevel(int level) {
  if (level < kMinSeverity) {
    level = kMinSeverity;
  } else if (level > kMaxSeverity) {
    level = kMaxSeverity;
  }
  max_log_level_.store(level, std::memory_order_relaxed);
}

bool LogEngine::shouldLog(Severity severity) const {
  return severityEnabled(static_cast<int>(severity),
                         max_log_level_.load(std::memory_order_relaxed));
}

bool LogEngine::onlyShow() const {
  return only_show_.load(std::memory_order_relaxed);
}

void LogEngine::setOnlyShow(bool only_show) {
  only_show_.store(only_show, std::memory_order_relaxed);
}

bool LogEngine::colorPrint() const {
  return color_print_.load(std::memory_order_relaxed);
}

void LogEngine::setColorPrint(bool color_print) {
  color_print_.store(color_print, std::memory_order_relaxed);
}

std::string LogEngine::logFile() const {
  std::lock_guard<std::mutex> lock(log_file_mutex_);
  return log_file_;
}

void LogEngine::setLogFile(const std::string& file_name) {
  std::lock_guard<std::mutex> lock(log_file_mutex_);
  log_file_ = file_name;
}

void LogEngine::setLogCallback(LogCallback callback) {
  // 接线回调 sink：callback_sink_ 挂在 logger_（屏幕）上，回调将收到完整屏幕行
  // （head + body + tail）。callback 存储于 callback_sink_ 内部（atomic），此处
  // 不再保留 LogEngine 级副本。
  if (callback_sink_ != nullptr) {
    callback_sink_->set_callback(callback);
  }
}

int LogEngine::parseMaxLogLevelValue(const std::string& value,
                                     int default_para) {
  std::string env_var = value;
  // 转大写后匹配。
  std::transform(env_var.begin(), env_var.end(), env_var.begin(),
                 [](unsigned char c) { return static_cast<char>(::toupper(c)); });
  if (env_var == "0" || env_var == "ERROR") {
    return 0;
  } else if (env_var == "1" || env_var == "WARNING") {
    return 1;
  } else if (env_var == "2" || env_var == "API_TRACE") {
    return 2;
  } else if (env_var == "3" || env_var == "INFO") {
    return 3;
  } else if (env_var == "4" || env_var == "DEBUG" || env_var == "DEBUG1" ||
             env_var == "DEBUG-1" || env_var == "DEBUG_1") {
    return 4;
  } else if (env_var == "5" || env_var == "DEBUG2" || env_var == "DEBUG-2" ||
             env_var == "DEBUG_2") {
    return 5;
  } else if (env_var == "6" || env_var == "DEBUG3" || env_var == "DEBUG-3" ||
             env_var == "DEBUG_3") {
    return 6;
  } else if (env_var == "7" || env_var == "DEBUG4" || env_var == "DEBUG-4" ||
             env_var == "DEBUG_4") {
    return 7;
  }
  return default_para;
}

LogMessage::LogMessage(std::string file, int line, Severity severity)
    : file_(file), line_(line), severity_(severity) {}

LogMessage::~LogMessage() {
  // 引擎未存活（未构造完成或已析构）时不发射，避免访问已销毁单例。
  if (g_log_engine_alive_flag.load(std::memory_order_relaxed) !=
      kLogEngineAliveMagic) {
    return;
  }
  LogEngine::instance().Emit(severity_, file_, line_, stream_.str());
}

std::stringstream& LogMessage::stream() { return stream_; }

// 装配并输出一条日志。屏幕 = head + body + tail；文件 = body + tail（无 head）。
// severity 门控提到函数开头：被抑制级别的日志直接返回，不做 head/body/tail 装配，
// 也不触发 cnrtGetDevice / getTime 副作用。NDEBUG 下不输出 tail。
void LogEngine::Emit(Severity severity, const std::string& file, int line,
                     const std::string& message) {
  static std::mutex log_mutex;

  // 提前门控：被抑制级别的日志不装配、不输出。
  if (static_cast<int>(severity) >
      max_log_level_.load(std::memory_order_relaxed)) {
    return;
  }

  const bool is_colored = color_print_.load(std::memory_order_relaxed);
  std::stringstream file_str;
  std::stringstream cout_str;

  // head 只进屏幕（文件无 head）。
  cout_str << fmt::format(
      "{datetime}[{module}][{severity}][{pid}][Card:{card}]: ",
      fmt::arg("datetime", getTime()),
      fmt::arg("module",
               is_colored
                   ? fmt::to_string(fmt::styled(
                         kProjectDisplayName,
                         fmt::emphasis::bold |
                             fmt::fg(fmt::terminal_color::yellow)))
                   : kProjectDisplayName),
      fmt::arg("severity", formatSeverity(severity, is_colored)),
      fmt::arg("pid", getpid_()),
      fmt::arg("card", []() {
        int dev_index = -1;
        cnrtGetDevice(&dev_index);
        return dev_index;
      }()));

  // body：文件与屏幕均有。
  file_str << message;
  cout_str << message;

#ifndef NDEBUG
  // tail：文件恒无色，屏幕按 color_print_ 决定彩色。
  int pid = getpid_();
#if defined(WINDOWS) || defined(WIN32)
  int tid = GetCurrentThreadId();
#else
  int tid = syscall(SYS_gettid);
#endif
  std::stringstream realId;
  if (pid == tid) {
    realId << pid;
  } else {
    realId << "{" << tid << "}";
  }
  file_str << "  "
           << "[ " << file << ":" << line << "  pid:" << realId.str() << "]";
  if (is_colored) {
    cout_str << "  " << kGreen << "[ " << file << ":" << line
             << "  pid:" << realId.str() << "]" << kReset;
  } else {
    cout_str << "  "
             << "[ " << file << ":" << line << "  pid:" << realId.str()
             << "]";
  }
#endif

  std::string file_ss = file_str.str();
  std::string cout_ss = cout_str.str();
  clearEnter(&file_ss);
  clearEnter(&cout_ss);

  std::lock_guard<std::mutex> lock(log_mutex);
  // 屏幕走 logger_（ConsoleSink + callback_sink_），文件走 file_logger_
  // （FileSink）。payload 为预先装配好的 cout_ss / file_ss，经 string_view_t
  // 原样传给 spdlog（logger.h 的非模板 log(level, string_view_t) 重载），
  // sink 逐字节写出（endl 语义）。
  logger_->log(static_cast<spdlog::level::level_enum>(static_cast<int>(severity)),
               spdlog::string_view_t(cout_ss.data(), cout_ss.size()));
  if (!only_show_.load(std::memory_order_relaxed)) {
    file_logger_->log(
        static_cast<spdlog::level::level_enum>(static_cast<int>(severity)),
        spdlog::string_view_t(file_ss.data(), file_ss.size()));
  }
}

}  // namespace log
}  // namespace mluop

extern "C" {

mluOpStatus_t MLUOP_WIN_API mluOpSetMaxLogLevel(MluOpLogLevel level) {
  mluop::log::LogEngine::instance().setMaxLogLevel(static_cast<int>(level));
  return MLUOP_STATUS_SUCCESS;
}

mluOpStatus_t MLUOP_WIN_API mluOpSetLogOnlyShow(int only_show) {
  mluop::log::LogEngine::instance().setOnlyShow(only_show != 0);
  return MLUOP_STATUS_SUCCESS;
}

mluOpStatus_t MLUOP_WIN_API mluOpSetLogColorPrint(int color) {
  mluop::log::LogEngine::instance().setColorPrint(color != 0);
  return MLUOP_STATUS_SUCCESS;
}

mluOpStatus_t MLUOP_WIN_API mluOpSetLogFile(const char* file_name) {
  if (file_name == nullptr) {
    return MLUOP_STATUS_BAD_PARAM;
  }
  mluop::log::LogEngine::instance().setLogFile(std::string(file_name));
  return MLUOP_STATUS_SUCCESS;
}

mluOpStatus_t MLUOP_WIN_API mluOpSetLogCallback(MluOpLogCallback callback) {
  mluop::log::LogEngine::instance().setLogCallback(
      reinterpret_cast<mluop::log::LogCallback>(callback));
  return MLUOP_STATUS_SUCCESS;
}

}  // extern "C"
