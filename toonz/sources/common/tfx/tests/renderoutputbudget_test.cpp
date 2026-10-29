#include "renderoutputbudget.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

template <class F>
void rejects(F operation, const char *message) {
  try {
    operation();
    require(false, message);
  } catch (const std::bad_alloc &) {
  } catch (...) {
    require(false, "invalid budget estimate did not throw std::bad_alloc");
  }
}

void outputModesAndBoundaries() {
  struct Mode {
    int bpp;
    bool stereo;
    bool field;
    std::uint64_t cost;
  };
  // A 3 x 5 raster has 15 pixels without row padding. These totals include the
  // outputs and clones retained by normal, stereo, and interlaced rendering.
  const Mode modes[] = {{32, false, false, 120}, {64, false, false, 240},
                        {128, false, false, 480}, {32, false, true, 180},
                        {64, false, true, 360}, {128, false, true, 720},
                        {32, true, false, 240}, {64, true, false, 480},
                        {128, true, false, 960}, {128, true, true, 960},
                        {24, false, false, 120}};
  for (const Mode &mode : modes) {
    const auto frames = [&](std::uint64_t budget) {
      return RenderOutputBudget::framesPerWindow(
          budget, 3, 5, mode.bpp, mode.stereo, mode.field);
    };
    require(frames(1) == 1, "oversized frame was not admitted for progress");
    require(frames(mode.cost - 1) == 1, "below-cost budget admitted extra frames");
    require(frames(mode.cost) == 1, "exact one-frame budget miscounted");
    require(frames(mode.cost + 1) == 1, "one spare byte admitted another frame");
    require(frames(mode.cost * 2 - 1) == 1, "partial second frame was admitted");
    require(frames(mode.cost * 2) == 2, "exact two-frame budget miscounted");
    require(frames(mode.cost * 2 + 1) == 2, "two-frame boundary miscounted");

    std::size_t previous = 1;
    for (std::uint64_t budget = 1; budget <= mode.cost * 11; ++budget) {
      const std::size_t count = frames(budget);
      require(count >= previous, "increasing budget reduced admitted frames");
      require(count >= 1, "finite budget prevented all progress");
      if (count > 1 || mode.cost <= budget) {
        require(count <= budget / mode.cost, "admitted outputs exceed budget");
        require(mode.cost * count <= budget, "output memory invariant failed");
        require(budget - mode.cost * count < mode.cost,
                "budget left room for an additional frame");
      }
      previous = count;
    }
  }
}

void invalidDimensionsAndOverflow() {
  const auto frames = [](int lx, int ly, int bpp, bool stereo, bool field) {
    return RenderOutputBudget::framesPerWindow(
        (std::numeric_limits<std::uint64_t>::max)(), lx, ly, bpp, stereo, field);
  };
  rejects([&] { frames(0, 10, 32, false, false); }, "zero width accepted");
  rejects([&] { frames(10, 0, 32, false, false); }, "zero height accepted");
  rejects([&] { frames(-1, 10, 32, false, false); }, "negative width accepted");
  rejects([&] { frames(10, -1, 32, false, false); }, "negative height accepted");

  const int maximum = (std::numeric_limits<int>::max)();
  // Each axis is representable, but either pixel bytes or retained copies can
  // overflow the total. Neither case may wrap into an unlimited admission.
  rejects([&] { frames(maximum, maximum, 128, false, false); },
          "pixel-byte overflow accepted");
  rejects([&] { frames(maximum, maximum, 32, false, false); },
          "retained-clone overflow accepted");
  rejects([&] { frames(maximum, maximum, 32, true, false); },
          "stereo output overflow accepted");
  rejects([&] { frames(maximum, maximum, 32, false, true); },
          "interlaced output overflow accepted");

  // This product exceeds 32 bits but remains a valid 64-bit estimate. The
  // estimator must not narrow before division; raster allocation validates its
  // own per-buffer constraints separately.
  require(frames(65536, 65536, 128, true, false) > 1,
          "wide valid output estimate was narrowed");
}

void unlimitedAndSizeLimit() {
  const std::size_t unlimited = (std::numeric_limits<std::size_t>::max)();
  require(RenderOutputBudget::framesPerWindow(0, 3, 5, 128, true, true) ==
              unlimited,
          "zero budget did not disable admission limit");
  require(RenderOutputBudget::framesPerWindow(0, 0, 0, 32, false, false) ==
              unlimited,
          "unlimited mode unexpectedly requires an output estimate");

  const std::uint64_t quotient = (std::numeric_limits<std::uint64_t>::max)() / 8;
  const std::size_t expected =
      quotient > unlimited ? unlimited : static_cast<std::size_t>(quotient);
  require(RenderOutputBudget::framesPerWindow(
              (std::numeric_limits<std::uint64_t>::max)(), 1, 1, 32, false,
              false) == expected,
          "large frame count did not preserve or clamp to size_t capacity");
}

}  // namespace

int main() {
  outputModesAndBoundaries();
  invalidDimensionsAndOverflow();
  unlimitedAndSizeLimit();
  std::cout << "render output budget checks passed\n";
  return 0;
}
