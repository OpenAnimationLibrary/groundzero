#pragma once

#include "celnoise.h"

#include <cmath>
#include <cstdint>

// Weather for the Particles FX: depth layers, turbulence and collision with
// a control image. Each is off at its default, and none draws from the
// particles' random sequences, so scenes that do not use them render exactly
// as before.
namespace ParticleWeather {

enum class Collision { Die = 0, Settle = 1, Bounce = 2 };

// A particle's depth in [0, 1] (0 nearest), from its seed, among `layers`
// equally spaced layers; one layer is no depth.
inline double depthOf(int seed, int layers) {
  if (layers <= 1) return 0.0;
  std::uint32_t h = std::uint32_t(seed) * 2654435761u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return double(h % std::uint32_t(layers)) / double(layers - 1);
}

// The factor for a particle at a depth, from 1 for the nearest layer to
// `far` for the farthest.
inline double depthFactor(double depth, double far) {
  return 1.0 + (far - 1.0) * depth;
}

// A drifting noise push at a position and frame, in pixels.
inline void turbulence(double strength, double size, double speed, double frame,
                       int seed, double x, double y, double &dx, double &dy) {
  dx = dy = 0.0;
  if (!(strength > 0.0) || !std::isfinite(strength)) return;
  CelNoise::Settings noise;
  noise.type = CelNoise::Smooth;
  noise.thickness =
      std::isfinite(size) ? std::fmax(1.0, std::fmin(100.0, size)) : 1.0;
  noise.offset = std::isfinite(frame * speed) ? frame * speed : 0.0;
  noise.seed   = std::uint32_t(seed) * 2654435761u + 0x6a09e667u;
  dx           = strength * (2.0 * CelNoise::sample(noise, x, y) - 1.0);
  noise.seed   = std::uint32_t(seed) * 2654435761u + 0xbb67ae85u;
  dy           = strength * (2.0 * CelNoise::sample(noise, x, y) - 1.0);
}

struct Motion {
  double x, y, vx, vy;
  bool settled;
  int lifetime;
};

// What a particle does on hitting the collision image, having moved from
// (oldX, oldY): it ends, rests where it was, or bounces back up.
inline void collide(Collision mode, double oldX, double oldY, Motion &m) {
  switch (mode) {
  case Collision::Die:
    m.lifetime = 0;
    break;
  case Collision::Settle:
    m.x = oldX, m.y = oldY;
    m.vx = m.vy = 0.0;
    m.settled   = true;
    break;
  case Collision::Bounce:
    m.x = oldX, m.y = oldY;
    m.vx *= 0.6;
    m.vy = -m.vy * 0.4;
    break;
  }
}

}  // namespace ParticleWeather
