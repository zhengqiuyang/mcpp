// mcpp/version.hpp — version macros for embedders.
#pragma once

#define MCPP_VERSION_MAJOR 0
#define MCPP_VERSION_MINOR 4
#define MCPP_VERSION_PATCH 0
#define MCPP_VERSION_STRING "0.4.0"

namespace mcpp {
inline constexpr const char* kVersion = MCPP_VERSION_STRING;
}
