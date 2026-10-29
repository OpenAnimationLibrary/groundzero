#include "../flowdisplacefx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using FlowDisplace::Color;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}
bool same(const Color &a, const Color &b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

struct Image {
  int w, h;
  std::vector<Color> pixels;
  Image(int w_, int h_, Color c = Color())
      : w(w_), h(h_), pixels(size_t(w_) * h_, c) {}
  Color &at(int x, int y) { return pixels[size_t(y) * w + x]; }
  const Color &at(int x, int y) const { return pixels[size_t(y) * w + x]; }
};

Image pattern(int w, int h) {
  Image image(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      image.at(x, y) = Color{(x % 7) / 6.0, (y % 5) / 4.0, 0.5, 1};
  return image;
}

std::vector<Color> displace(const Image &image, const Image *field,
                            const Image *matte, const FlowDisplace::Settings &s,
                            double frame = 0, double ox = 0, double oy = 0) {
  std::vector<Color> out;
  require(FlowDisplace::render(image.pixels,
                               field ? field->pixels : std::vector<Color>(),
                               matte ? matte->pixels : std::vector<Color>(),
                               image.w, image.h, ox, oy, s, frame, out),
          "Displacement was canceled");
  return out;
}

void testFlow() {
  // A constant field of (+1, -0.5): the image moves by strength times it
  const Image image = pattern(40, 30);
  const Image field(40, 30, Color{1.0, 0.25, 0, 1});
  FlowDisplace::Settings s;
  s.strength     = 4;
  const auto out = displace(image, &field, nullptr, s);
  for (int y = 6; y < 24; ++y)
    for (int x = 8; x < 32; ++x)
      require(same(out[size_t(y) * 40 + x], image.at(x - 4, y + 2)),
              "A constant field did not move the image by its strength");
  // A neutral field, or no field, leaves the image as it is
  const Image neutral(40, 30, Color{0.5, 0.5, 0.5, 1});
  const auto still = displace(image, &neutral, nullptr, s);
  const auto none  = displace(image, nullptr, nullptr, s);
  for (size_t i = 0; i < still.size(); ++i)
    require(same(still[i], image.pixels[i]) && same(none[i], image.pixels[i]),
            "A neutral field moved the image");
  // Invert moves it the other way
  s.invert        = true;
  const auto back = displace(image, &field, nullptr, s);
  require(same(back[size_t(15) * 40 + 20], image.at(24, 13)),
          "Invert did not reverse the motion");
}

void testMap() {
  const Image image = pattern(40, 30);
  const Image white(40, 30, Color{1, 1, 1, 1});
  FlowDisplace::Settings s;
  s.mode         = FlowDisplace::Mode::Map;
  s.strength     = 3;
  s.direction    = 90;
  const auto out = displace(image, &white, nullptr, s);
  // White pushes the full strength along the direction (down, +y)
  require(approx(out[size_t(15) * 40 + 20].r, image.at(20, 12).r, 1e-9) &&
              approx(out[size_t(15) * 40 + 20].g, image.at(20, 12).g, 1e-9),
          "A white map did not push along its direction");
}

void testMatte() {
  // The matte weights the motion: none outside it, all of it inside
  const Image image = pattern(40, 30);
  const Image field(40, 30, Color{1.0, 0.5, 0, 1});
  Image matte(40, 30);
  for (int y = 0; y < 30; ++y)
    for (int x = 20; x < 40; ++x) matte.at(x, y) = Color{1, 1, 1, 1};
  FlowDisplace::Settings s;
  s.strength     = 3;
  s.useMatte     = true;
  const auto out = displace(image, &field, &matte, s);
  require(same(out[size_t(15) * 40 + 10], image.at(10, 15)),
          "Pixels outside the matte moved");
  require(same(out[size_t(15) * 40 + 30], image.at(27, 15)),
          "Pixels inside the matte did not move");
  // Without a connected matte nothing moves when the matte is required
  const auto nothing = displace(image, &field, nullptr, s);
  require(same(nothing[size_t(15) * 40 + 30], image.at(30, 15)),
          "A missing matte did not stop the motion");
}

void testNoise() {
  // Noise alone gives motion that changes with time, repeatably
  const Image image = pattern(48, 40);
  FlowDisplace::Settings s;
  s.strength       = 0;
  s.noiseAmplitude = 3;
  s.noiseSize      = 8;
  s.noiseSpeed     = 0.25;
  const auto f0    = displace(image, nullptr, nullptr, s, 0);
  const auto f0b   = displace(image, nullptr, nullptr, s, 0);
  const auto f4    = displace(image, nullptr, nullptr, s, 4);
  bool moved = false, changes = false;
  for (size_t i = 0; i < f0.size(); ++i) {
    require(same(f0[i], f0b[i]), "Noise is not repeatable");
    moved   = moved || !same(f0[i], image.pixels[i]);
    changes = changes || !same(f0[i], f4[i]);
  }
  require(moved, "Noise did not move the image");
  require(changes, "Noise did not change over time");
}

void testTiling() {
  const Image image = pattern(64, 56);
  Image field(64, 56);
  for (int y = 0; y < 56; ++y)
    for (int x = 0; x < 64; ++x)
      field.at(x, y) = Color{0.5 + 0.4 * std::sin(x * 0.2),
                             0.5 + 0.4 * std::cos(y * 0.15), 0, 1};
  Image matte(64, 56);
  for (int y = 0; y < 56; ++y)
    for (int x = 0; x < 64; ++x)
      matte.at(x, y) = Color{0, 0, 0, std::min(1.0, x / 40.0)};
  FlowDisplace::Settings s;
  s.strength       = 4.5;
  s.noiseAmplitude = 2;
  s.noiseSize      = 9;
  s.useMatte       = true;
  const int halo   = FlowDisplace::halo(s);
  const auto full  = displace(image, &field, &matte, s, 3);
  for (int x0 : {0, 21, 44})
    for (int y0 : {0, 19, 38}) {
      const int tw = 19, th = 17;
      Image iw(tw + 2 * halo, th + 2 * halo), fw(iw.w, iw.h), mw(iw.w, iw.h);
      for (int y = 0; y < iw.h; ++y)
        for (int x = 0; x < iw.w; ++x) {
          const int sx = x0 - halo + x, sy = y0 - halo + y;
          if (sx < 0 || sy < 0 || sx >= image.w || sy >= image.h) continue;
          iw.at(x, y) = image.at(sx, sy);
          fw.at(x, y) = field.at(sx, sy);
          mw.at(x, y) = matte.at(sx, sy);
        }
      const auto part = displace(iw, &fw, &mw, s, 3, x0 - halo, y0 - halo);
      for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
          const int sx = x0 + x, sy = y0 + y;
          if (sx < halo || sy < halo || sx >= image.w - halo ||
              sy >= image.h - halo)
            continue;
          require(same(full[size_t(sy) * image.w + sx],
                       part[size_t(y + halo) * iw.w + x + halo]),
                  "A tile differs from the full frame at " +
                      std::to_string(sx) + "," + std::to_string(sy));
        }
    }
}

// The FX on rasters of each depth
class Source final : public TStandardRasterFx {
  Image m_image;
  std::string m_name;

public:
  Source(const Image &image, const std::string &name)
      : m_image(image), m_name(name) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return m_name;
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_image.w, m_image.h);
    return true;
  }
  template <class Pixel>
  void fill(const TRasterPT<Pixel> &raster, TPointD pos) {
    raster->clear();
    for (int y = 0; y < raster->getLy(); ++y)
      for (int x = 0; x < raster->getLx(); ++x) {
        const int sx = x + int(pos.x), sy = y + int(pos.y);
        if (sx < 0 || sy < 0 || sx >= m_image.w || sy >= m_image.h) continue;
        const Color &c = m_image.at(sx, sy);
        raster->pixels(y)[x] =
            PixelConverter<Pixel>::from(TPixelD(c.r, c.g, c.b, c.a));
      }
  }
  void doCompute(TTile &tile, double, const TRenderSettings &) override {
    if (TRaster32P r = tile.getRaster())
      fill<TPixel32>(r, tile.m_pos);
    else if (TRaster64P r = tile.getRaster())
      fill<TPixel64>(r, tile.m_pos);
    else if (TRasterFP r = tile.getRaster())
      fill<TPixelF>(r, tile.m_pos);
  }
  void compute(TTile &tile, double frame, const TRenderSettings &s) override {
    doCompute(tile, frame, s);
  }
};

TDoubleParamP numeric(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}

template <class Pixel>
void testFx() {
  const Image image = pattern(44, 38);
  Image field(44, 38);
  for (int y = 0; y < 38; ++y)
    for (int x = 0; x < 44; ++x)
      field.at(x, y) = Color{0.5 + 0.3 * std::sin(x * 0.3), 0.6, 0, 1};
  TFxP fx     = new FlowDisplaceFx;
  TFxP source = new Source(image, "displace-source"),
       flow   = new Source(field, "displace-field");
  require(fx->connect("Source", source.getPointer()) &&
              fx->connect("Displacement", flow.getPointer()),
          "Cannot connect the inputs");
  numeric(*fx, "strength")->setDefaultValue(5);
  numeric(*fx, "noiseAmplitude")->setDefaultValue(1.5);
  auto &displaceFx = static_cast<FlowDisplaceFx &>(*fx);
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  auto render    = [&](TPointD pos, int w, int h, double frame) {
    TRasterPT<Pixel> raster(w, h);
    TTile tile(raster, pos);
    displaceFx.doCompute(tile, frame, settings);
    return raster;
  };
  const TRasterPT<Pixel> whole = render(TPointD(0, 0), 44, 38, 2);
  const TRasterPT<Pixel> part  = render(TPointD(13, 9), 15, 13, 2);
  for (int y = 0; y < 13; ++y)
    for (int x = 0; x < 15; ++x)
      require(part->pixels(y)[x] == whole->pixels(y + 9)[x + 13],
              "An FX tile differs from the whole frame");
  // With drifting noise, frames have their own cache keys; without, they
  // share one
  require(displaceFx.getAlias(1, settings) != displaceFx.getAlias(2, settings),
          "Noisy frames share a cache key");
  numeric(*fx, "noiseAmplitude")->setDefaultValue(0);
  require(displaceFx.getAlias(1, settings) == displaceFx.getAlias(2, settings),
          "Still frames have different cache keys");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFlow();
    testMap();
    testMatte();
    testNoise();
    testTiling();
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: vector field, grey map, matte, noise, tiling and "
                 "pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
