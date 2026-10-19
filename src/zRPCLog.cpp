/*
 * @file   zRPCLog.cpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief Routing of the messages zRPC logs from its own threads
 *
 * @copyright Jonathan Haws -- 2026
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <iostream>
#include <mutex>
#include "zRPC.hpp"

namespace zRPC
{
namespace
{
/**
 * @brief Guards the current log handler
 */
std::mutex &logMutex()
{
  static std::mutex mutex;
  return mutex;
}

/**
 * @brief The current log handler, initially one that writes to std::cerr
 */
LogHandler &currentHandler()
{
  static LogHandler handler = [](LogLevel level, const std::string &message)
  {
    // One write per message so lines from different threads do not mix
    std::cerr << (std::string("zRPC ") +
                  (level == LogLevel::Error ? "error: " : "warning: ") +
                  message + "\n")
              << std::flush;
  };
  return handler;
}
}  // namespace

LogHandler setLogHandler(LogHandler handler)
{
  std::lock_guard<std::mutex> lock(logMutex());
  std::swap(currentHandler(), handler);
  return handler;
}

namespace detail
{
void log(LogLevel level, const std::string &message)
{
  LogHandler handler;
  {
    std::lock_guard<std::mutex> lock(logMutex());
    handler = currentHandler();
  }

  // Called outside the lock, so a handler may itself call setLogHandler()
  if (handler)
  {
    handler(level, message);
  }
}
}  // namespace detail

}  // namespace zRPC
