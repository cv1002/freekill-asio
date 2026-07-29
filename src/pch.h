// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// 老规矩头文件这块偷懒

// std
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <list>
#include <deque>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <functional>
#include <memory>
#include <utility>
#include <optional>
#include <variant>
#include <tuple>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <stdexcept>
#include <limits>
#include <type_traits>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <regex>
#include <random>

// C++20 std
#include <ranges>

// spdlog
#include <spdlog/spdlog.h>

// libcbor
#include <cbor.h>

// boost networking library
#include <boost/asio.hpp>
#include <boost/beast.hpp>

#ifdef __APPLE__
#include <pthread.h>
// macOS 仅支持单参数；Linux 为两参数。便于本机编译测试
inline int fk_pthread_setname_np(pthread_t, const char *name) {
  return ::pthread_setname_np(name);
}
#define pthread_setname_np(thread, name) fk_pthread_setname_np((thread), (name))
#endif

#define OPENSSL_API_COMPAT 0x10101000L
