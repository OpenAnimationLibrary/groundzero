#include "../celcolorkeymath.h"
#include "../celdistance.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void close(double actual, double expected, double tolerance = 1e-10) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
    throw std::runtime_error("Numerical result differs from analytical value: " +
                             std::to_string(actual) + " vs " +
                             std::to_string(expected));
}

template <typename Function> void invalid(Function function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument &) {
    rejected = true;
  } catch (const std::length_error &) {
    rejected = true;
  }
  require(rejected, "Invalid input was accepted");
}

// Independent, exhaustive nearest-seed oracle. It enumerates every source and
// evaluates the metric directly, rather than calling the transform or sharing
// its lower-envelope/chamfer implementation.
void verifyDistance(const std::vector<std::uint8_t> &seeds, int width, int height,
                    CelDistance::Metric metric, bool outside) {
  CelDistance::Field field;
  require(CelDistance::transform(seeds, width, height, metric, field, nullptr,
                                 outside),
          "Distance computation failed");
  require(field.width == width && field.height == height &&
              field.distance.size() == seeds.size() &&
              field.nearest.size() == seeds.size(),
          "Distance dimensions changed");
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      double best = std::numeric_limits<double>::infinity();
      int label = -1;
      for (std::size_t i = 0; i < seeds.size(); ++i) {
        if (!seeds[i]) continue;
        const int dx = std::abs(x - static_cast<int>(i) % width);
        const int dy = std::abs(y - static_cast<int>(i) / width);
        double distance = 0.0;
        switch (metric) {
        case CelDistance::Metric::Box: distance = std::max(dx, dy); break;
        case CelDistance::Metric::Approximate:
          distance = std::max(dx, dy) + std::min(dx, dy) / 3.0;
          break;
        case CelDistance::Metric::Euclidean:
          distance = std::sqrt(static_cast<double>(dx) * dx +
                               static_cast<double>(dy) * dy);
          break;
        }
        if (distance < best) {
          best = distance;
          label = static_cast<int>(i);
        }
      }
      if (outside) {
        const int border = std::min(std::min(x + 1, width - x),
                                    std::min(y + 1, height - y));
        if (border < best) {
          best = border;
          label = -1;
        }
      }
      const int index = y * width + x;
      if (std::isfinite(best))
        close(field.distance[index], best);
      else
        require(std::isinf(field.distance[index]), "Empty field became finite");
      require(field.nearest[index] == label,
              "Nearest source or equal-distance priority is incorrect");
    }
  }
}

void distances() {
  const CelDistance::Metric metrics[] = {CelDistance::Metric::Box,
                                         CelDistance::Metric::Approximate,
                                         CelDistance::Metric::Euclidean};
  for (const auto metric : metrics) {
    for (unsigned pattern = 0; pattern < 512; ++pattern) {
      std::vector<std::uint8_t> mask(9);
      for (int i = 0; i < 9; ++i) mask[i] = (pattern >> i) & 1;
      verifyDistance(mask, 3, 3, metric, false);
      verifyDistance(mask, 3, 3, metric, true);
    }
    // Wide and tall fields exercise the separable transform in both axes and
    // include ties between seeds in the same row and in different rows.
    std::vector<std::uint8_t> mask(35, 0);
    mask[1] = mask[9] = mask[18] = mask[31] = 1;
    verifyDistance(mask, 7, 5, metric, false);
    verifyDistance(mask, 5, 7, metric, true);
    verifyDistance({1, 0, 0, 0, 1}, 5, 1, metric, false);
    verifyDistance({1, 0, 0, 0, 1}, 1, 5, metric, false);
  }
  CelDistance::Field field;
  // Squared distances exceed 32-bit range. The midpoint must still select
  // the lower source index, including in the vertical envelope pass.
  std::vector<std::uint8_t> longLine(100001, 0);
  longLine.front() = longLine.back() = 1;
  for (bool vertical : {false, true}) {
    require(CelDistance::transform(longLine, vertical ? 1 : 100001,
                                   vertical ? 100001 : 1,
                                   CelDistance::Metric::Euclidean, field),
            "Long Euclidean field failed");
    close(field.distance[50000], 50000.0);
    close(field.distance[50001], 49999.0);
    require(field.nearest[50000] == 0 && field.nearest[50001] == 100000,
            "Large squared distance lost nearest-source tie precision");
  }
  require(CelDistance::transform({}, 0, 19, CelDistance::Metric::Euclidean,
                                 field),
          "Empty dimensions rejected");
  require(field.width == 0 && field.height == 19 && field.nearest.empty(),
          "Empty dimensions not preserved");
  int canceled = 1;
  field.distance = {4.0};
  field.nearest = {0};
  require(!CelDistance::transform({1}, 1, 1, CelDistance::Metric::Box, field,
                                  &canceled) &&
              field.distance.empty() && field.nearest.empty() &&
              field.width == 0 && field.height == 0,
          "Canceled distance field was retained");
  invalid([&] { CelDistance::transform({1}, 2, 1, CelDistance::Metric::Box, field); });
  invalid([&] { CelDistance::transform({}, -1, 0, CelDistance::Metric::Box, field); });
  invalid([&] {
    CelDistance::transform({}, std::numeric_limits<int>::max(), 2,
                           CelDistance::Metric::Euclidean, field);
  });
  invalid([&] {
    CelDistance::transform({1}, 1, 1, static_cast<CelDistance::Metric>(99), field);
  });
}

void regionEdges() {
  using CelDistance::Direction;
  using CelDistance::Metric;
  const std::vector<std::uint8_t> point = {0, 0, 0, 0, 1, 0, 0, 0, 0};
  std::vector<std::uint8_t> adjusted;
  std::vector<double> coverage;
  const Metric metrics[] = {Metric::Box, Metric::Approximate, Metric::Euclidean};
  for (const Metric metric : metrics) {
    require(CelDistance::thin(point, 3, 3, 0.0, metric, adjusted) &&
                adjusted == point,
            "Zero thin is not identity");
    require(CelDistance::thin(point, 3, 3, 1.0, metric, adjusted),
            "Expansion failed");
    require(adjusted == (metric == Metric::Box
                             ? std::vector<std::uint8_t>(9, 1)
                             : std::vector<std::uint8_t>{0, 1, 0, 1, 1, 1, 0, 1, 0}),
            "Unit expansion has incorrect support");
    std::vector<std::uint8_t> solid(25, 1);
    require(CelDistance::thin(solid, 5, 5, -1.0, metric, adjusted),
            "Contraction failed");
    for (int y = 0; y < 5; ++y)
      for (int x = 0; x < 5; ++x)
        require(adjusted[y * 5 + x] == (x > 0 && x < 4 && y > 0 && y < 4),
                "Contraction ignored off-image background");
    require(CelDistance::thin(point, 3, 3, 4000.0, metric, adjusted) &&
                adjusted == std::vector<std::uint8_t>(9, 1),
            "Large expansion failed");
    require(CelDistance::thin(point, 3, 3, -4000.0, metric, adjusted) &&
                adjusted == std::vector<std::uint8_t>(9, 0),
            "Large contraction failed");
    for (Direction direction : {Direction::Inside, Direction::Around,
                                Direction::Outside}) {
      require(CelDistance::feather(point, 3, 3, 0.0, metric, direction, coverage),
              "Zero feather failed");
      for (std::size_t i = 0; i < point.size(); ++i) close(coverage[i], point[i]);
      require(CelDistance::feather(std::vector<std::uint8_t>(9, 0), 3, 3,
                                   4000.0, metric, direction, coverage),
              "Empty feather failed");
      for (double value : coverage) close(value, 0.0);
    }
    require(CelDistance::feather(point, 3, 3, 1.0, metric, Direction::Inside,
                                 coverage),
            "Inside feather failed");
    close(coverage[4], 0.5);
    close(coverage[1], 0.0);
    require(CelDistance::feather(point, 3, 3, 1.0, metric, Direction::Outside,
                                 coverage),
            "Outside feather failed");
    close(coverage[4], 1.0);
    close(coverage[1], 0.5);
    require(CelDistance::feather(point, 3, 3, 1.0, metric, Direction::Around,
                                 coverage),
            "Around feather failed");
    close(coverage[4], 2.0 / 3.0);
    close(coverage[1], 1.0 / 3.0);
    require(CelDistance::feather(point, 3, 3, 0.25, metric, Direction::Around,
                                 coverage),
            "Fractional feather failed");
    close(coverage[4], 5.0 / 6.0);
    close(coverage[1], 1.0 / 6.0);
    require(CelDistance::feather(solid, 5, 5, 1.0, metric, Direction::Inside,
                                 coverage),
            "Solid feather failed");
    close(coverage[0], 0.5);
    close(coverage[12], 1.0);
  }
  // Aliasing the thin input/output is supported and does not erase the source.
  adjusted = point;
  require(CelDistance::thin(adjusted, 3, 3, 1.0, Metric::Box, adjusted) &&
              adjusted == std::vector<std::uint8_t>(9, 1),
          "In-place region adjustment failed");
  int canceled = 1;
  require(!CelDistance::thin(point, 3, 3, 1.0, Metric::Box, adjusted, &canceled) &&
              adjusted.empty(),
          "Canceled region adjustment was retained");
  require(!CelDistance::feather(point, 3, 3, 1.0, Metric::Box, Direction::Around,
                                coverage, &canceled) && coverage.empty(),
          "Canceled feather was retained");
  invalid([&] {
    CelDistance::thin(point, 3, 3, std::numeric_limits<double>::infinity(),
                      Metric::Box, adjusted);
  });
  invalid([&] {
    CelDistance::feather(point, 3, 3, -1.0, Metric::Box, Direction::Around,
                         coverage);
  });
}

void conversions() {
  using CelKey::ColorSpace;
  using CelKey::RGB;
  const RGB red = {{1, 0, 0}}, white = {{1, 1, 1}}, black = {{0, 0, 0}};
  RGB value = CelKey::convert(red, ColorSpace::HSV);
  close(value[0], 0.0); close(value[1], 1.0); close(value[2], 1.0);
  value = CelKey::convert(RGB{{0, 1, 0}}, ColorSpace::HSV);
  close(value[0], 1.0 / 3.0);
  value = CelKey::convert(RGB{{0, 0, 1}}, ColorSpace::HSV);
  close(value[0], 2.0 / 3.0);
  value = CelKey::differences(RGB{{0.99, 1, 1}}, RGB{{0.01, 1, 1}},
                               ColorSpace::HSV);
  close(value[0], 0.04);
  value = CelKey::differences(RGB{{0.99, 0, 1}}, RGB{{0.01, 1, 1}},
                               ColorSpace::HSV);
  close(value[0], 0.0); close(value[1], 1.0);

  // Standard D65 sRGB primary coordinates, rounded independently to 4 decimals.
  value = CelKey::convert(red, ColorSpace::Lab76);
  close(value[0], 53.2408, 0.0001);
  close(value[1], 80.0925, 0.0001);
  close(value[2], 67.2032, 0.0001);
  value = CelKey::convert(white, ColorSpace::Lab76);
  close(value[0], 100.0, 0.0001);
  close(value[1], 0.0, 0.0001);
  close(value[2], 0.0, 0.0001);
  value = CelKey::convert(black, ColorSpace::Lab94);
  for (double component : value) close(component, 0.0);
  value = CelKey::differences(RGB{{60, 30, 40}}, RGB{{50, 0, 0}},
                               ColorSpace::Lab76);
  close(value[0], 0.1); close(value[1], 30.0 / 255.0); close(value[2], 40.0 / 255.0);
  value = CelKey::differences(RGB{{60, 60, 80}}, RGB{{50, 30, 40}},
                               ColorSpace::Lab94);
  close(value[0], 0.1);
  close(value[1], 50.0 / 3.25 / 255.0);
  close(value[2], 0.0, 1e-9);
  value = CelKey::differences(RGB{{50, -40, 30}}, RGB{{50, 30, 40}},
                               ColorSpace::Lab94);
  close(value[1], 0.0);
  close(value[2], std::sqrt(5000.0) / 1.75 / 255.0);

  value = CelKey::convert(red, ColorSpace::YUV);
  close(value[0], 0.299); close(value[1], -0.14713); close(value[2], 0.615);
  value = CelKey::convert(red, ColorSpace::YCrCb);
  close(value[0], 0.299); close(value[1], 0.5); close(value[2], -0.299 / 1.772);
  value = CelKey::convert(RGB{{0, 0, 1}}, ColorSpace::YCrCb);
  close(value[0], 0.114); close(value[1], -0.114 / 1.402); close(value[2], 0.5);
}

void matching() {
  using CelKey::ColorSpace;
  using CelKey::Entry;
  using CelKey::Matcher;
  using CelKey::Precision;
  using CelKey::RGB;
  using CelKey::Settings;
  std::vector<Entry> entries(25);
  for (int i = 0; i < 25; ++i) {
    entries[i].enabled = true;
    entries[i].color = {{i / 24.0, 0.25, 0.75}};
  }
  Settings settings;
  for (ColorSpace space : {ColorSpace::RGB, ColorSpace::HSV, ColorSpace::Lab76,
                           ColorSpace::Lab94, ColorSpace::YUV, ColorSpace::YCrCb}) {
    settings.space = space;
    Matcher matcher(entries.data(), entries.size(), settings);
    for (int i = 0; i < 25; ++i)
      require(matcher.firstMatch(entries[i].color, 1.0) == i,
              "A color slot failed to match in a comparison space");
    require(matcher.firstMatch(RGB{{0, 0, 0}}, 1.0) == -1,
            "Unselected color matched");
  }
  settings = Settings();
  entries[0].enabled = false;
  entries[24].color = entries[0].color;
  require(Matcher(entries.data(), entries.size(), settings)
                  .firstMatch(entries[0].color, 1.0) == 24,
          "Disabled entry won overlap priority");
  entries[0].enabled = true;
  require(Matcher(entries.data(), entries.size(), settings)
                  .firstMatch(entries[0].color, 1.0) == 0,
          "First matching entry did not win");
  require(Matcher(nullptr, 0, settings).firstMatch(RGB{{0, 0, 0}}, 1.0) == -1,
          "Empty selection matched");

  Entry key;
  key.enabled = true;
  key.color = {{0, 1, 0}};
  require(Matcher(&key, 1, settings).firstMatch(RGB{{0, 0.25, 0}}, 0.25) == 0,
          "Native premultiplied source was not unpremultiplied");
  settings.premultiplied = true;
  require(Matcher(&key, 1, settings).firstMatch(RGB{{0, 0.25, 0}}, 0.25) == -1,
          "Premultiplied option did not change comparison");
  settings = Settings();
  require(Matcher(&key, 1, settings).firstMatch(key.color, 0.0) == -1,
          "Zero-alpha source matched");
  require(Matcher(&key, 1, settings)
                  .firstMatch(RGB{{0, std::numeric_limits<double>::quiet_NaN(), 0}},
                              1.0) == -1,
          "Nonfinite source matched");
  require(Matcher(&key, 1, settings)
                  .firstMatch(key.color, std::numeric_limits<double>::infinity()) == -1,
          "Nonfinite alpha matched");
  key.color = {{0, 0, 0}};
  settings.threshold = 0.2;
  require(Matcher(&key, 1, settings).firstMatch(RGB{{0.3, 0, 0}}, 1.0) == 0,
          "Shared scalar threshold failed");
  settings.perComponent = true;
  settings.components = {{0.2, 0.2, 0.2}};
  require(Matcher(&key, 1, settings).firstMatch(RGB{{0.3, 0, 0}}, 1.0) == -1,
          "Shared component threshold used scalar distance");
  settings.perColor = true;
  key.components = {{0.4, 0.0, 0.0}};
  require(Matcher(&key, 1, settings).firstMatch(RGB{{0.3, 0, 0}}, 1.0) == 0,
          "Per-entry component threshold ignored");
  settings.perComponent = false;
  key.threshold = 0.1;
  require(Matcher(&key, 1, settings).firstMatch(RGB{{0.3, 0, 0}}, 1.0) == -1,
          "Per-entry scalar threshold ignored");

  // Adjacent values remain distinct at native precision but merge when the
  // selected lower comparison precision maps them to the same code value.
  settings = Settings();
  key.color = {{0.25, 0.25, 0.25}};
  const RGB fine = {{0.25 + 4e-6, 0.25, 0.25}};
  require(Matcher(&key, 1, settings).firstMatch(fine, 1.0) == -1,
          "Full float comparison discarded fine differences");
  settings.precision = Precision::Bit16;
  require(Matcher(&key, 1, settings).firstMatch(fine, 1.0) == 0,
          "Float-to-16 comparison reduction failed");
  const RGB coarser = {{0.2505, 0.25, 0.25}};
  require(Matcher(&key, 1, settings).firstMatch(coarser, 1.0) == -1,
          "16-bit comparison lost excess precision");
  settings.precision = Precision::Bit8;
  require(Matcher(&key, 1, settings).firstMatch(coarser, 1.0) == 0,
          "Float-to-8 comparison reduction failed");
  settings.sourceBits = 16;
  require(Matcher(&key, 1, settings).firstMatch(coarser, 1.0) == 0,
          "16-to-8 comparison reduction failed");
  settings.sourceBits = 8;
  settings.precision = Precision::Bit16;
  require(Matcher(&key, 1, settings).firstMatch(coarser, 1.0) == 0,
          "Lower-precision mode increased native precision");
  settings = Settings();
  key.color = {{2.0, 0.1, 0.5}};
  require(Matcher(&key, 1, settings).firstMatch(key.color, 1.0) == 0,
          "Full float comparison clipped HDR");

  // A prepared snapshot is immutable even if the original parameter buffer
  // changes, and repeated Lab comparisons do not destructively convert keys.
  settings.space = ColorSpace::Lab76;
  key.color = {{1, 0, 0}};
  const Matcher stable(&key, 1, settings);
  key.color = {{0, 1, 0}};
  for (int i = 0; i < 20; ++i) {
    require(stable.firstMatch(RGB{{0, 0, 1}}, 1.0) == -1,
            "Unselected Lab sample matched");
    require(stable.firstMatch(RGB{{1, 0, 0}}, 1.0) == 0,
            "Lab key state changed between samples");
  }
  settings.sourceBits = 12;
  invalid([&] { Matcher rejected(&key, 1, settings); });
  settings = Settings();
  settings.threshold = std::numeric_limits<double>::quiet_NaN();
  invalid([&] { Matcher rejected(&key, 1, settings); });
  settings = Settings();
  invalid([&] { Matcher rejected(nullptr, 1, settings); });
}

}  // namespace

int main() {
  try {
    distances();
    regionEdges();
    conversions();
    matching();
  } catch (const std::exception &error) {
    std::cerr << "Cel color-key math: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Cel color-key math checks passed\n";
  return 0;
}
