// For the standalone RemoteGamePadMediaTest.cpp only: what Cemu's precompiled header gives
// RemoteGamePadMediaApple.cpp (standard headers, fmt, cemuLog_log).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>

enum class LogType { Force = 63 };
template <typename... Args>
void cemuLog_log(LogType, fmt::format_string<Args...> f, Args&&... args)
{
	printf("[log] %s\n", fmt::format(f, std::forward<Args>(args)...).c_str());
}
