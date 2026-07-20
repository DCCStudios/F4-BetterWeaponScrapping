#pragma once

#pragma warning(push)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#pragma warning(pop)

#define DLLEXPORT __declspec(dllexport)

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = F4SE::log;
using namespace std::literals;

#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
