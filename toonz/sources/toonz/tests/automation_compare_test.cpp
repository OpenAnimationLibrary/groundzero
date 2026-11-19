#include "automation.h"
#include "automation_compare.h"

#include <QJsonArray>

#include <cmath>
#include <stdexcept>
#include <vector>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<uchar> image(int width, int height, std::vector<uchar> pixel) {
  std::vector<uchar> data;
  for (int index = 0; index < width * height; ++index)
    data.insert(data.end(), pixel.begin(), pixel.end());
  return data;
}

void set(std::vector<uchar>& data, int width, int x, int y,
         std::vector<uchar> pixel) {
  std::copy(pixel.begin(), pixel.end(), data.begin() + (y * width + x) * 4);
}

CompareOutput run(const std::vector<uchar>& left,
                  const std::vector<uchar>& right, int width, int height,
                  bool premultiplied, int threshold) {
  CompareInput input;
  input.reference     = left.data();
  input.candidate     = right.data();
  input.width         = width;
  input.height        = height;
  input.region        = QRect(0, 0, width, height);
  input.premultiplied = premultiplied;
  input.threshold     = threshold;
  input.gridColumns   = 3;
  input.gridRows      = 2;
  return compareImages(input);
}

// Hand-computed: a 4 x 3 image, two separate changed pixels.
void metricsByHand() {
  std::vector<uchar> left  = image(4, 3, {10, 20, 30, 255});
  std::vector<uchar> right = left;
  set(right, 4, 0, 0, {13, 20, 30, 255});
  set(right, 4, 3, 2, {10, 20, 30, 250});
  const CompareOutput out    = run(left, right, 4, 3, false, 0);
  const QJsonObject channels = out.metrics.value("channels").toObject();
  const QJsonObject red      = channels.value("r").toObject();
  require(red.value("max_abs").toInt() == 3 &&
              red.value("sum_abs").toInt() == 3 &&
              red.value("sum_sq").toInt() == 9,
          "Red sums are wrong");
  require(red.value("mean_abs").toDouble() == 3.0 / 12.0,
          "Mean is not one binary64 division");
  require(red.value("rmse").toDouble() == std::sqrt(9.0 / 12.0),
          "RMSE is not the binary64 square root");
  require(out.changed == 2 && out.maxAbsAlpha == 5,
          "Changed count or alpha max");
  require(out.metrics.value("changed_ppm").toInt() == 166666,
          "Changed ppm is not floored");
  const QJsonObject percentiles = out.metrics.value("percentiles").toObject();
  require(percentiles.value("p50").toInt() == 0 &&
              percentiles.value("p95").toInt() == 5 &&
              percentiles.value("p100").toInt() == 5,
          "Nearest-rank percentiles are wrong");
  const QJsonObject components = out.regions.value("components").toObject();
  require(components.value("total").toInt() == 2, "Components not separate");
  const QJsonArray items = components.value("items").toArray();
  require(
      items.at(0).toObject().value("box").toObject().value("x").toInt() == 0,
      "Equal areas are not ordered by y then x");
  const QJsonArray cells =
      out.regions.value("grid").toObject().value("cells").toArray();
  // Columns split 4 as [0,1) [1,2) [2,4); rows split 3 as [0,1) [1,3).
  require(cells.size() == 6 && cells.at(0).toArray().at(0).toInt() == 1 &&
              cells.at(5).toArray().at(0).toInt() == 1,
          "Grid cell boundaries are wrong");
  require(!out.metrics.value("identical_straight").toBool(), "Not identical");
}

// Premultiplied mode ignores colour under zero alpha and rounds half up.
void premultipliedMode() {
  std::vector<uchar> left  = image(2, 1, {200, 0, 0, 0});
  std::vector<uchar> right = image(2, 1, {10, 0, 0, 0});
  require(run(left, right, 2, 1, true, 0).changed == 0,
          "Colour under zero alpha counted in premultiplied mode");
  require(run(left, right, 2, 1, false, 0).changed == 2,
          "Straight mode ignored colour under zero alpha");
  for (int value = 0; value < 256; ++value)
    for (int alpha = 0; alpha < 256; ++alpha) {
      std::vector<uchar> one  = image(1, 1, {uchar(value), 0, 0, uchar(alpha)});
      std::vector<uchar> zero = image(1, 1, {0, 0, 0, uchar(alpha)});
      const int expected      = int(std::floor(value * alpha / 255.0 + 0.5));
      const CompareOutput out = run(one, zero, 1, 1, true, 255);
      const int got           = out.metrics.value("channels")
                                    .toObject()
                                    .value("r")
                                    .toObject()
                                    .value("max_abs")
                                    .toInt();
      require(got == expected, "Premultiplied rounding is not round half up");
    }
}

void cancellation() {
  const std::vector<uchar> data = image(4, 200, {1, 2, 3, 4});
  CompareInput input;
  input.reference = data.data();
  input.candidate = data.data();
  input.width     = 4;
  input.height    = 200;
  input.region    = QRect(0, 0, 4, 200);
  std::atomic<bool> stop{true};
  input.cancel = &stop;
  require(compareImages(input).canceled, "A set cancel flag did not stop");
}

void pixelDigest() {
  const std::vector<uchar> data = image(1, 1, {1, 2, 3, 4});
  // SHA-256 of "GZRGBA8\n" 00000001 00000001 01020304.
  require(
      pixelSha256(data.data(), 1, 1) ==
          "ee3068e99ba1c6021d7e43fd7df49eb3b06976c5bcb5fb216088aa982d4bed4b",
      "Pixel digest framing changed");
}

}  // namespace

void runAutomationCompareTests() {
  metricsByHand();
  premultipliedMode();
  cancellation();
  pixelDigest();
}

}  // namespace GroundzeroAutomation
