// Traffic Engineering Fabric - global traffic-engineering intent runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace tef {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

// Durable/wire format versions are independent of the product version.
// They advance only when the on-disk or on-wire encoding changes incompatibly.
inline constexpr std::uint32_t kDurableFormatVersion = 1;
inline constexpr std::uint32_t kWireProtocolVersion = 1;

}  // namespace tef
