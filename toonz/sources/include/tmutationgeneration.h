#pragma once

#ifndef T_MUTATION_GENERATION_INCLUDED
#define T_MUTATION_GENERATION_INCLUDED

#include <cstdint>
#include <limits>

namespace TMutation {

//! Advance a native mutation counter without wrapping an exhausted generation.
//! Consumers must reject UINT64_MAX rather than treating it as a stable token.
inline void advanceGeneration(std::uint64_t& generation) noexcept {
  if (generation != (std::numeric_limits<std::uint64_t>::max)()) ++generation;
}

}  // namespace TMutation

#endif
