// Regression test of the Ghost Fade of the onion skin (toonz/ghostfade.h,
// doc/ghost_fade.md): the default settings leave every ghost as it was, the
// opacity multiplier scales and clamps, and the step keeps every Nth relative
// ghost and all the fixed ones.

#include "toonz/ghostfade.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void testDefaults() {
  const GhostFade::Settings settings;
  require(settings.isDefault(), "defaults: not reported as default");
  require(settings.opacity == 1.0 && settings.tint && settings.step == 1,
          "defaults: not opacity 1, tinted, every frame");
  // The opacity of every ghost is unchanged, bit for bit: the viewer looks as
  // before the Ghost Fade
  for (int i = 0; i <= 1000; ++i) {
    const double opacity = i / 1000.0;
    require(GhostFade::ghostOpacity(opacity, settings.opacity) == opacity,
            "defaults: the opacity of a ghost changed");
  }
  // The automatic fade of the paper thickness: 1 - fade, fade in
  // [0.35, 0.95]
  for (double fade : {0.35, 0.47, 0.6, 0.95})
    require(GhostFade::ghostOpacity(1.0 - fade, 1.0) == 1.0 - fade,
            "defaults: the automatic fade changed");
  // Every ghost is drawn
  for (int drow = -30; drow <= 30; ++drow) {
    if (drow == 0) continue;
    require(GhostFade::showsGhost(drow, false, settings.step),
            "defaults: a relative ghost is hidden");
    require(GhostFade::showsGhost(drow, true, settings.step),
            "defaults: a fixed ghost is hidden");
  }
  require(GhostFade::sanitized(settings).isDefault(),
          "defaults: sanitizing changed the defaults");
}

void testOpacity() {
  require(std::abs(GhostFade::ghostOpacity(0.6, 0.5) - 0.3) < 1e-12,
          "opacity: half of 0.6 is not 0.3");
  require(GhostFade::ghostOpacity(0.6, 0.0) == 0.0,
          "opacity: 0 does not hide the ghost");
  require(std::abs(GhostFade::ghostOpacity(0.4, 2.0) - 0.8) < 1e-12,
          "opacity: double of 0.4 is not 0.8");
  require(GhostFade::ghostOpacity(0.65, 2.0) == 1.0,
          "opacity: not clamped to 1");
  require(GhostFade::ghostOpacity(0.5, -1.0) == 0.0,
          "opacity: not clamped to 0");
  // Monotonic in the multiplier
  double last = -1.0;
  for (int i = 0; i <= 200; ++i) {
    const double opacity = GhostFade::ghostOpacity(0.45, i / 100.0);
    require(opacity >= last, "opacity: not monotonic");
    last = opacity;
  }
}

void testStep() {
  // Every 4th frame: -12, -8, -4, 4, 8, 12 of the relative ghosts
  int shown = 0;
  for (int drow = -12; drow <= 12; ++drow) {
    if (drow == 0) continue;
    const bool expected = drow % 4 == 0;
    require(GhostFade::showsGhost(drow, false, 4) == expected,
            "step: wrong relative ghost for every 4th frame");
    if (GhostFade::showsGhost(drow, false, 4)) ++shown;
    // The fixed markers are always drawn
    require(GhostFade::showsGhost(drow, true, 4), "step: a fixed ghost hidden");
  }
  require(shown == 6, "step: not six ghosts in +-12 frames, every 4th");
  require(GhostFade::showsGhost(-3, false, 3) &&
              !GhostFade::showsGhost(-2, false, 3),
          "step: wrong back ghost for every 3rd frame");
  // A step below 1 draws them all
  require(GhostFade::showsGhost(5, false, 0) &&
              GhostFade::showsGhost(-7, false, -2),
          "step: a step below 1 hides ghosts");
}

void testSanitized() {
  GhostFade::Settings settings;
  settings.opacity          = 5.0;
  settings.step             = 0;
  GhostFade::Settings fixed = GhostFade::sanitized(settings);
  require(fixed.opacity == GhostFade::maxOpacity,
          "sanitized: opacity above max");
  require(fixed.step == 1, "sanitized: step below 1");
  settings.opacity = -0.5;
  settings.step    = 1000;
  fixed            = GhostFade::sanitized(settings);
  require(fixed.opacity == 0.0, "sanitized: negative opacity");
  require(fixed.step == GhostFade::maxStep, "sanitized: step above max");
  settings.opacity = std::numeric_limits<double>::quiet_NaN();
  require(GhostFade::sanitized(settings).opacity == 0.0,
          "sanitized: NaN opacity");
  settings.opacity = 0.7;
  settings.step    = 3;
  settings.tint    = false;
  fixed            = GhostFade::sanitized(settings);
  require(fixed.opacity == 0.7 && fixed.step == 3 && !fixed.tint,
          "sanitized: changed settings in range");
  require(!fixed.isDefault(), "sanitized: changed settings reported default");
}

}  // namespace

int main() {
  try {
    testDefaults();
    testOpacity();
    testStep();
    testSanitized();
  } catch (const std::exception &e) {
    std::cerr << "ghostfade_test: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "ok: ghost fade" << std::endl;
  return 0;
}
