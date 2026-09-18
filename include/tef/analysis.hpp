// Traffic Engineering Fabric - objective accounting and alternatives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "tef/model.hpp"

namespace tef {

// One objective term's contribution. Lower total score is better. Components are
// reported in objective-profile order so that two runs with the same inputs
// produce the same component vector.
struct ObjectiveComponent {
  ObjectiveTerm term = ObjectiveTerm::satisfy_minimums;
  std::int64_t weight = 0;
  std::int64_t raw = 0;       // unweighted magnitude in the term's natural unit
  std::int64_t weighted = 0;  // weight * raw (checked; saturates at the numeric bound)
  std::string unit;

  friend bool operator==(const ObjectiveComponent&, const ObjectiveComponent&) noexcept = default;
};

// A concrete alternative the solver considered and did not take, with the
// primary reason. Alternatives are what make the decision auditable.
struct RejectedAlternative {
  std::string subject;
  std::string reason;
  std::int64_t delta = 0;
  std::int64_t alternative_score = 0;

  friend bool operator==(const RejectedAlternative&, const RejectedAlternative&) noexcept = default;
  friend std::strong_ordering operator<=>(const RejectedAlternative& a,
                                          const RejectedAlternative& b) noexcept {
    if (auto c = a.reason <=> b.reason; c != 0) return c;
    return a.subject <=> b.subject;
  }
};

}  // namespace tef
