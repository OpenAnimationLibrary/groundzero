#pragma once

#include "toonz/rasterbrushengine.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace RasterBrush {
namespace Detail {

// Versioned source-color calibration, separate from native paint math. Profile
// SHA256: 5991c8d8fcb628dad5d052d9341df8a32bd3c7a794c913a8ede8eae4b34b4545.
// Exact ICC matrix/TRC, a 33-cube U16 Lab grid, and fixed-domain tetrahedral
// interpolation reproduce all 544 public U16 conversions from source 5.3.4.1.
// See kernel-convergence-notes.md for the observations and standards provenance.
struct QuantizedPickupLab {
  std::array<std::uint16_t, 3> encoded{};
  std::array<double, 3> physical{};
};
using PickupLabConverter = bool (*)(const Pixel &, QuantizedPickupLab &);
using PickupLabU16Converter = bool (*)(const std::array<std::uint16_t, 3> &,
                                      QuantizedPickupLab &);

struct PickupLabCalibration {
  PickupLabConverter convert = nullptr;
  constexpr bool available() const { return convert != nullptr; }
  const char *reason = "Source pickup Lab calibration unavailable";
  PickupLabU16Converter convertU16 = nullptr;
};

class SourcePickupLabTransform {
public:
  using Code = std::array<std::uint16_t, 3>;
  static constexpr unsigned side = 33;
  static constexpr size_t vertexCount = size_t(side) * side * side;
  static constexpr size_t tableBytes = vertexCount * sizeof(Code);
  // Conservative resource-accounting units for all matrix/Lab grid vertices
  // and TRC generation, separate from the cheap warmed lookup/checkpoint cost.
  static constexpr size_t initializationWork = vertexCount * 128 + side * 256;

  SourcePickupLabTransform() {
    // One immutable process cache, independent of stroke/replicated-track
    // storage. Reserve its complete fixed bound before allocating. Construction
    // computes 33 TRC vertices and exactly 33^3 matrix/Lab conversions.
    // No fixture-dependent table or external profile asset is retained.
    if (!m_budget.reserve(tableBytes)) throw std::bad_alloc();
    m_table.reset(new Table);
    std::array<double, side> linear{};
    for (unsigned i = 0; i < side; ++i) {
      const unsigned code = unsigned(std::floor(i * 65535.0 / 32 + .5));
      if (code == 65535) {
        linear[i] = 1;
        continue;
      }
      const auto position = fixedDomain(std::uint64_t(code) * 4095);
      const unsigned index = unsigned(position >> 16);
      const unsigned fraction = unsigned(position & 65535);
      const unsigned lo = curveCode(index), hi = curveCode(index + 1);
      linear[i] = (lo + (((hi - lo) * fraction + 32768) >> 16)) / 65535.0;
    }
    for (unsigned r = 0; r < side; ++r)
      for (unsigned g = 0; g < side; ++g)
        for (unsigned b = 0; b < side; ++b) {
          // Signed 16.16 ICC XYZ tags; the Lab reference white is the
          // color-management D50 constant, not the quantized profile wtpt tag.
          const double x = (28576 * linear[r] + 25239 * linear[g] +
                            9375 * linear[b]) / 65536 / .9642;
          const double y = (14581 * linear[r] + 46983 * linear[g] +
                            3972 * linear[b]) / 65536;
          const double z = (912 * linear[r] + 6361 * linear[g] +
                            46788 * linear[b]) / 65536 / .8249;
          const double fx = labCurve(x), fy = labCurve(y), fz = labCurve(z);
          (*m_table)[offset(r, g, b)] = {{
              quantize((116 * fy - 16) * 65535 / 100),
              quantize((500 * (fx - fy) + 128) * 257),
              quantize((200 * (fy - fz) + 128) * 257)}};
        }
  }
  ~SourcePickupLabTransform() {
    m_table.reset();
    m_budget.release(tableBytes);
  }
  Code convert(const Code &rgb) const {
    std::array<unsigned, 3> index{}, fraction{}, order{{0, 1, 2}};
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto position = fixedDomain(std::uint64_t(rgb[axis]) * 32);
      index[axis] = unsigned(position >> 16);
      fraction[axis] = unsigned(position & 65535);
    }
    // An independently specified tetrahedron: visit the fractional axes in
    // descending order and integrate the three successive vertex differences.
    for (unsigned i = 1; i < 3; ++i) {
      const unsigned axis = order[i];
      unsigned position = i;
      while (position && fraction[axis] > fraction[order[position - 1]]) {
        order[position] = order[position - 1];
        --position;
      }
      order[position] = axis;
    }
    const Code base = (*m_table)[offset(index[0], index[1], index[2])];
    Code previous = base;
    std::array<std::int64_t, 3> weighted{};
    for (unsigned axis : order) {
      index[axis] = std::min(32u, index[axis] + 1);
      const Code next = (*m_table)[offset(index[0], index[1], index[2])];
      for (unsigned channel = 0; channel < 3; ++channel)
        weighted[channel] += (std::int64_t(next[channel]) - previous[channel]) *
                             fraction[axis];
      previous = next;
    }
    Code result{};
    for (unsigned channel = 0; channel < 3; ++channel) {
      const std::int64_t rest = weighted[channel] + 32769;
      const auto delta = floorDivide65536(rest + floorDivide65536(rest));
      result[channel] = std::uint16_t(std::max<std::int64_t>(
          0, std::min<std::int64_t>(65535, base[channel] + delta)));
    }
    return result;
  }
  size_t chargedBytes() const { return m_budget.used(); }

private:
  using Table = std::array<Code, vertexCount>;
  MemoryBudget m_budget{tableBytes};
  std::unique_ptr<Table> m_table;
  static size_t offset(unsigned r, unsigned g, unsigned b) {
    return (size_t(r) * side + g) * side + b;
  }
  static std::uint64_t fixedDomain(std::uint64_t value) {
    return value + (value + 32767) / 65535;
  }
  static std::int64_t floorDivide65536(std::int64_t value) {
    // Define negative division explicitly; signed right-shift differs across
    // older C++ implementations and round-toward-zero is not equivalent here.
    return value >= 0 ? value / 65536 : -((-value + 65535) / 65536);
  }
  static std::uint16_t quantize(double value) {
    return std::uint16_t(std::floor(std::max(0.0, std::min(65535.0, value)) + .5));
  }
  static unsigned curveCode(unsigned index) {
    const double x = index / 4095.0;
    const double linear = x <= .04045 ? x / 12.92
        : std::pow((x + .055) / 1.055, 2.4);
    return unsigned(std::floor(linear * 65535 + .5));
  }
  static double labCurve(double value) {
    constexpr double d = 6.0 / 29;
    return value > d * d * d ? std::cbrt(value)
                             : value / (3 * d * d) + 4.0 / 29;
  }
};

inline std::atomic<bool> &sourcePickupLabReadyFlag() {
  static std::atomic<bool> ready{false};
  return ready;
}
inline bool sourcePickupLabReady() {
  return sourcePickupLabReadyFlag().load(std::memory_order_acquire);
}
inline size_t sourcePickupLabPreparationWork() {
  // This query does not allocate or initialize the transform. Two racing cold
  // callers may both charge its fixed cost; neither may bypass the charge.
  return sourcePickupLabReady() ? 0 : SourcePickupLabTransform::initializationWork;
}
inline const SourcePickupLabTransform &sourcePickupLabTransform() {
  static const SourcePickupLabTransform transform;
  sourcePickupLabReadyFlag().store(true, std::memory_order_release);
  return transform;
}
inline bool sourcePickupLabFromU16(const SourcePickupLabTransform::Code &rgb,
                                  QuantizedPickupLab &lab) {
  lab.encoded = sourcePickupLabTransform().convert(rgb);
  lab.physical[0] = lab.encoded[0] * 100.0 / 65535;
  for (unsigned axis = 1; axis < 3; ++axis) {
    const double value = lab.encoded[axis] / 257.0 - 128;
    // The pinned public Lab XML serializer has asymmetric positive a/b
    // normalization. Preserve its measured metric, including this 127/128
    // factor; neither ICC V2 physical values nor raw ordered() are equivalent.
    lab.physical[axis] = value > 0 ? value * (127.0 / 128) : value;
  }
  return true;
}
inline bool convertSourcePickupLab(const Pixel &mean, QuantizedPickupLab &lab) {
  if (!std::isfinite(mean.a) || mean.a < 0 || mean.a > 1) return false;
  SourcePickupLabTransform::Code rgb{};
  const std::array<double, 3> channels{{mean.r, mean.g, mean.b}};
  for (unsigned axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(channels[axis]) || channels[axis] < 0 ||
        channels[axis] > mean.a) return false;
    // Public U16 component writes truncate all 544 measured means. Unassociate
    // premultiplied storage before the RGB profile conversion, retaining alpha
    // independently in the cumulative pickup returned to the renderer.
    rgb[axis] = mean.a > 0
        ? std::uint16_t(std::floor(std::min(1.0, channels[axis] / mean.a) * 65535))
        : 0;
  }
  return sourcePickupLabFromU16(rgb, lab);
}
constexpr PickupLabCalibration sourcePickupLabCalibration() {
  return {convertSourcePickupLab,
          "Krita 5.3.4.1; sRGB-elle V2; 33-cube LabV4 U16; public XML metric",
          sourcePickupLabFromU16};
}

enum class PickupCountBasis { Attempted, Accepted };
enum class PickupMeanStorage {
  FloatingPoint,
  QuantizedStraightRgba8,
  QuantizedPremultipliedRgba8
};
enum class PickupDecision {
  Collecting,
  Converged,
  WorkLimit,
  CalibrationUnavailable,
  InvalidConfiguration,
  InvalidSample,
  InvalidCalibration
};

// Allocation-free cumulative-mean controller for the measured 16-sample batch
// policy. The minimum/window rule and modern clipping count basis are supplied
// independently; neither is inferred from the convergence observations.
class AdaptivePickupSampling {
public:
  static constexpr unsigned batchSize = 16;
  static constexpr unsigned maximumAttempts = 1000000;

  AdaptivePickupSampling(unsigned minimum, unsigned attemptLimit,
                         PickupLabCalibration calibration,
                         PickupCountBasis countBasis,
                         PickupMeanStorage storage = PickupMeanStorage::FloatingPoint)
      : m_minimum(minimum), m_attemptLimit(attemptLimit),
        m_nextCheckpoint(minimum > batchSize ? minimum - batchSize : 0),
        m_calibration(calibration), m_countBasis(countBasis), m_storage(storage) {
    if (minimum < 80 || minimum > attemptLimit ||
        attemptLimit > maximumAttempts ||
        (countBasis != PickupCountBasis::Attempted &&
         countBasis != PickupCountBasis::Accepted) ||
        (storage != PickupMeanStorage::FloatingPoint &&
         storage != PickupMeanStorage::QuantizedStraightRgba8 &&
         storage != PickupMeanStorage::QuantizedPremultipliedRgba8) ||
        (storage != PickupMeanStorage::FloatingPoint && !calibration.convertU16))
      m_decision = PickupDecision::InvalidConfiguration;
    else if (!calibration.available())
      m_decision = PickupDecision::CalibrationUnavailable;
  }

  PickupDecision add(const Pixel &sample, bool accepted) {
    if (m_decision != PickupDecision::Collecting) return m_decision;
    if (m_attempts == m_attemptLimit) {
      m_decision = PickupDecision::WorkLimit;
      return m_decision;
    }
    ++m_attempts;
    if (accepted) {
      if (!valid(sample)) {
        m_decision = PickupDecision::InvalidSample;
        return m_decision;
      }
      if (m_storage != PickupMeanStorage::FloatingPoint) {
        // Byte storage is declared by the reader. Straight-byte input returns
        // associated Pixels, so accumulate its exact color*alpha products.
        // Associated-byte input already has quantized associated channels;
        // unassociating and rounding them would invent a second quantization.
        // Every sum (and the largest checkpoint numerator) is below 2^52 at
        // the million-attempt limit; uint64 arithmetic stays exact.
        const std::uint64_t alpha = channelCode(sample.a);
        m_alphaSum += alpha;
        const std::array<double, 3> channels{{sample.r, sample.g, sample.b}};
        for (unsigned axis = 0; axis < 3; ++axis) {
          if (m_storage == PickupMeanStorage::QuantizedPremultipliedRgba8)
            m_colorSum[axis] += channelCode(channels[axis]);
          else {
            const std::uint64_t color = sample.a > 0
                ? channelCode(channels[axis] / sample.a) : 0;
            m_colorSum[axis] += color * alpha;
          }
        }
      } else {
        if (!m_accepted) m_first = sample;
        else if (sample.r != m_first.r || sample.g != m_first.g ||
                 sample.b != m_first.b || sample.a != m_first.a)
          m_uniform = false;
        accumulate(sample.r, m_sum.r, m_compensation.r);
        accumulate(sample.g, m_sum.g, m_compensation.g);
        accumulate(sample.b, m_sum.b, m_compensation.b);
        accumulate(sample.a, m_sum.a, m_compensation.a);
      }
      ++m_accepted;
    }
    const unsigned count = m_countBasis == PickupCountBasis::Attempted
                               ? m_attempts
                               : m_accepted;
    if (count == m_nextCheckpoint) checkpoint();
    if (m_decision == PickupDecision::Collecting &&
        m_attempts == m_attemptLimit)
      m_decision = PickupDecision::WorkLimit;
    return m_decision;
  }

  PickupDecision decision() const { return m_decision; }
  unsigned attempts() const { return m_attempts; }
  unsigned accepted() const { return m_accepted; }
  unsigned minimum() const { return m_minimum; }

  bool result(Pixel &pixel) const {
    if (m_decision != PickupDecision::Converged || !m_accepted) return false;
    pixel = mean();
    return true;
  }

private:
  unsigned m_minimum, m_attemptLimit, m_nextCheckpoint;
  unsigned m_attempts = 0, m_accepted = 0;
  PickupLabCalibration m_calibration;
  PickupCountBasis m_countBasis;
  PickupMeanStorage m_storage;
  PickupDecision m_decision = PickupDecision::Collecting;
  Pixel m_sum{}, m_compensation{}, m_first{};
  bool m_uniform = true;
  std::uint64_t m_alphaSum = 0;
  std::array<std::uint64_t, 3> m_colorSum{};
  QuantizedPickupLab m_previous{};
  bool m_hasPrevious = false;

  static bool valid(const Pixel &p) {
    return std::isfinite(p.r) && std::isfinite(p.g) &&
           std::isfinite(p.b) && std::isfinite(p.a) && p.a >= 0 && p.a <= 1 &&
           p.r >= 0 && p.r <= p.a && p.g >= 0 && p.g <= p.a &&
           p.b >= 0 && p.b <= p.a;
  }
  static void accumulate(double value, double &sum, double &compensation) {
    const double adjusted = value - compensation;
    const double next = sum + adjusted;
    compensation = (next - sum) - adjusted;
    sum = next;
  }
  static std::uint64_t channelCode(double value) {
    return std::uint64_t(std::floor(std::min(1.0, value) * 65535 + .5));
  }
  Pixel mean() const {
    if (m_storage != PickupMeanStorage::FloatingPoint) {
      const std::uint64_t denominator = std::uint64_t(m_accepted) * 65535;
      const double colorDenominator =
          m_storage == PickupMeanStorage::QuantizedPremultipliedRgba8
              ? double(denominator) : double(denominator * 65535);
      return {m_colorSum[0] / colorDenominator,
              m_colorSum[1] / colorDenominator,
              m_colorSum[2] / colorDenominator,
              double(m_alphaSum) / denominator};
    }
    // The initial retained Dulling field is the unquantized pickup mean;
    // equal accepted pixels must return that original Pixel exactly, including
    // sub-U16 alpha. No storage quantization is inferred from Lab conversion.
    if (m_uniform) return m_first;
    const double scale = 1.0 / m_accepted;
    return {m_sum.r * scale, m_sum.g * scale, m_sum.b * scale,
            m_sum.a * scale};
  }
  void checkpoint() {
    if (!m_accepted) {
      m_decision = PickupDecision::InvalidSample;
      return;
    }
    QuantizedPickupLab current;
    bool converted;
    if (m_storage != PickupMeanStorage::FloatingPoint) {
      SourcePickupLabTransform::Code rgb{};
      const std::uint64_t multiplier =
          m_storage == PickupMeanStorage::QuantizedPremultipliedRgba8 ? 65535 : 1;
      for (unsigned axis = 0; axis < 3; ++axis)
        rgb[axis] = m_alphaSum
            ? std::uint16_t(m_colorSum[axis] * multiplier / m_alphaSum) : 0;
      // Exact associated sums are unassociated only for Lab conversion.
      // Integer division retains the measured truncation at channel boundaries.
      converted = m_calibration.convertU16(rgb, current);
    } else {
      converted = m_calibration.convert(mean(), current);
    }
    if (!converted) {
      m_decision = PickupDecision::InvalidCalibration;
      return;
    }
    for (double component : current.physical)
      if (!std::isfinite(component)) {
        m_decision = PickupDecision::InvalidCalibration;
        return;
      }
    if (m_hasPrevious) {
      double squared = 0;
      for (unsigned component = 0; component < 3; ++component) {
        const double delta = current.physical[component] -
                             m_previous.physical[component];
        squared += delta * delta;
      }
      // Strict measured boundary; no tolerance or epsilon is introduced.
      if (squared < 9) {
        m_decision = PickupDecision::Converged;
        return;
      }
    }
    m_previous = current;
    m_hasPrevious = true;
    m_nextCheckpoint = m_nextCheckpoint < m_minimum
                           ? m_minimum
                           : m_nextCheckpoint + batchSize;
  }
};

}  // namespace Detail
}  // namespace RasterBrush
