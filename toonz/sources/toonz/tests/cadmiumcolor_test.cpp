#include "cadmiumcolor.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}
bool sameRegions(const QVector<int> &actual, std::initializer_list<int> expected) {
  return actual.size() == static_cast<int>(expected.size()) &&
         std::equal(actual.cbegin(), actual.cend(), expected.begin());
}
Cadmium::NativeColor color(double red, double alpha = 1.0,
                          bool linear = false) {
  return {red, 0.23456789, -0.1251234, alpha, linear, true};
}
}

int main() {
  const QColor proxy(100, 50, 25, 255);
  Cadmium::Frame frame;
  frame.regionColors = {QColor(), proxy, proxy, proxy, proxy, proxy,
                        QColor(101, 50, 25, 255)};
  frame.nativeRegionColors = {{}, color(0.12345678), color(0.12345678),
                              color(0.12345679), color(0.12345678, 0.5),
                              color(0.12345678, 1.0, true), color(2.3456789)};
  require(sameRegions(Cadmium::matchingNativeRegions(frame, proxy), {1, 3, 4, 5}),
          "native precision, alpha or color-space collisions were collapsed");
  require(Cadmium::matchingNativeRegions(frame, QColor()).isEmpty(),
          "an invalid prediction resolved a native reference");
  require(sameRegions(
              Cadmium::matchingNativeRegions(frame, QColor(101, 50, 25, 255)), {6}),
          "a unique HDR reference was not resolved");
  require(Cadmium::matchingNativeRegions(frame, QColor(100, 50, 25, 254)).isEmpty(),
          "different proxy alpha values were treated as identical");

  frame.nativeRegionColors[1].valid = false;
  require(sameRegions(Cadmium::matchingNativeRegions(frame, proxy), {2, 3, 4, 5}),
          "an invalid first reference displaced the first valid occurrence");
  frame.nativeRegionColors.resize(4);
  require(sameRegions(Cadmium::matchingNativeRegions(frame, proxy), {2, 3}),
          "missing native entries were used as reference candidates");
  frame.regionColors.resize(3);
  require(sameRegions(Cadmium::matchingNativeRegions(frame, proxy), {2}),
          "native entries without proxy colors were used as candidates");

  frame.regionColors = {proxy, proxy, proxy, proxy, proxy};
  frame.nativeRegionColors = {color(std::numeric_limits<double>::quiet_NaN()),
                              color(std::numeric_limits<double>::infinity()),
                              color(0.1, -0.1), color(0.1, 1.1), color(4.321987)};
  require(sameRegions(Cadmium::matchingNativeRegions(frame, proxy), {4}),
          "invalid native samples were accepted or finite HDR was rejected");
  std::cout << "Cadmium native reference matching checks passed\n";
  return 0;
}
