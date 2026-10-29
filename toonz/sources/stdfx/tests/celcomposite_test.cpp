#include "../lightwrapfx.cpp"
#include "../edgeextendfx.cpp"
#include "../mattechokefx.cpp"
#include "../unmultiplyfx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using CelComposite::Color;

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

// A grey disk with an antialiased edge on transparency
Image disk(int w, int h, double r, Color c = Color{0.3, 0.3, 0.3, 1}) {
  Image image(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double d     = std::hypot(x + 0.5 - w / 2.0, y + 0.5 - h / 2.0);
      const double cover = std::max(0.0, std::min(1.0, r - d + 0.5));
      image.at(x, y)     = Color{c.r * cover, c.g * cover, c.b * cover, cover};
    }
  return image;
}

void testLightWrap() {
  const Image fg = disk(64, 64, 20);
  const Image bg(64, 64, Color{1, 0.9, 0.2, 1});
  CelComposite::WrapSettings s;
  s.width     = 6;
  s.softness  = 0;
  s.intensity = 1;
  std::vector<Color> out;
  require(CelComposite::lightWrap(fg.pixels, bg.pixels, 64, 64, s, out),
          "Light wrap canceled");
  const Color &centre = out[size_t(32) * 64 + 32];
  const Color &edge   = out[size_t(32) * 64 + 32 + 19];
  require(same(centre, fg.at(32, 32)), "Light reached the interior");
  require(edge.r > fg.at(51, 32).r + 0.3, "No light at the edge");
  require(approx(edge.a, fg.at(51, 32).a), "Screen changed the alpha");
  require(approx(out[0].a, 0) && approx(out[0].r, 0),
          "Light outside the foreground");
  // Halfway into the band, about half the light
  const double half = out[size_t(32) * 64 + 32 + 16].g;
  require(half > fg.at(48, 32).g && half < edge.g, "The band does not fade");
  // Add stays within the alpha
  s.blend     = CelComposite::WrapBlend::Add;
  s.intensity = 4;
  require(CelComposite::lightWrap(fg.pixels, bg.pixels, 64, 64, s, out),
          "Add canceled");
  for (const Color &c : out)
    require(c.r <= c.a + 1e-12 && c.g <= c.a + 1e-12,
            "Added light exceeds the alpha");
}

void testEdgeExtend() {
  // A red disk on a white background, flattened: the edge mixes both
  Image image = disk(40, 40, 12, Color{1, 0, 0, 1});
  CelComposite::ExtendSettings s;
  s.radius = 3;
  std::vector<Color> out;
  require(CelComposite::edgeExtend(image.pixels, 40, 40, s, out),
          "Edge extend canceled");
  for (size_t i = 0; i < out.size(); ++i) {
    const Color &c = out[i];
    require(approx(c.a, image.pixels[i].a), "Edge extend changed the alpha");
    if (c.a > 0)
      require(approx(c.g, 0) && approx(c.r, c.a, 1e-9),
              "An edge pixel kept a mixed colour");
  }
  // With alpha extended, the band around the disk becomes solid
  s.extendAlpha = true;
  require(CelComposite::edgeExtend(image.pixels, 40, 40, s, out),
          "Edge extend canceled");
  // (the last solid pixel on this row is 10 from the centre: 3 more is in)
  require(approx(out[size_t(20) * 40 + 20 + 13].a, 1),
          "Extended alpha did not fill the band");
  require(approx(out[size_t(20) * 40 + 20 + 18].a, 0),
          "Extended alpha went past the radius");
}

double area(const std::vector<Color> &image) {
  double sum = 0;
  for (const Color &c : image) sum += c.a;
  return sum;
}

void testChoke() {
  const Image image   = disk(60, 60, 15);
  const double before = area(image.pixels);
  for (double amount : {-3.0, 3.0}) {
    CelComposite::ChokeSettings s;
    s.amount  = amount;
    s.feather = 0.5;
    std::vector<Color> out;
    require(CelComposite::choke(image.pixels, 60, 60, s, out),
            "Choke canceled");
    // A disk of radius 15 + amount
    const double expected = 3.14159265358979 * (15 + amount) * (15 + amount);
    require(
        approx(area(out), expected, expected * 0.04),
        "The edge did not move by the amount: " + std::to_string(area(out)) +
            " instead of " + std::to_string(expected));
    require((amount > 0) == (area(out) > before), "Choke and spread swapped");
    // Colours keep their hue, including on spread pixels
    for (const Color &c : out)
      if (c.a > 1e-6)
        require(approx(c.r / c.a, 0.3, 1e-9), "A colour changed its hue");
  }
}

void testTiling() {
  const Image fg = disk(70, 64, 22);
  Image bg(70, 64);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 70; ++x)
      bg.at(x, y) = Color{x / 70.0, y / 64.0, 0.5, 1};
  CelComposite::WrapSettings wrap;
  wrap.width    = 5.5;
  wrap.softness = 3;
  CelComposite::ExtendSettings extend;
  extend.radius      = 4.5;
  extend.extendAlpha = true;
  CelComposite::ChokeSettings choke;
  choke.amount  = 2.5;
  choke.feather = 1.5;
  struct Case {
    std::string name;
    int halo;
    std::function<std::vector<Color>(const Image &, const Image &)> run;
  };
  const std::vector<Case> cases = {
      {"light wrap", CelComposite::wrapHalo(wrap),
       [&](const Image &f, const Image &b) {
         std::vector<Color> out;
         CelComposite::lightWrap(f.pixels, b.pixels, f.w, f.h, wrap, out);
         return out;
       }},
      {"edge extend", CelComposite::extendHalo(extend),
       [&](const Image &f, const Image &) {
         std::vector<Color> out;
         CelComposite::edgeExtend(f.pixels, f.w, f.h, extend, out);
         return out;
       }},
      {"choke", CelComposite::chokeHalo(choke),
       [&](const Image &f, const Image &) {
         std::vector<Color> out;
         CelComposite::choke(f.pixels, f.w, f.h, choke, out);
         return out;
       }}};
  for (const Case &c : cases) {
    const auto full = c.run(fg, bg);
    const int halo  = c.halo;
    for (int x0 : {0, 23, 46})
      for (int y0 : {0, 21, 42}) {
        const int tw = 19, th = 17;
        Image fw(tw + 2 * halo, th + 2 * halo), bw(fw.w, fw.h);
        for (int y = 0; y < fw.h; ++y)
          for (int x = 0; x < fw.w; ++x) {
            const int sx = x0 - halo + x, sy = y0 - halo + y;
            if (sx >= 0 && sy >= 0 && sx < fg.w && sy < fg.h) {
              fw.at(x, y) = fg.at(sx, sy);
              bw.at(x, y) = bg.at(sx, sy);
            }
          }
        const auto part = c.run(fw, bw);
        for (int y = 0; y < th; ++y)
          for (int x = 0; x < tw; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (sx < halo || sy < halo || sx >= fg.w - halo ||
                sy >= fg.h - halo)
              continue;
            require(same(full[size_t(sy) * fg.w + sx],
                         part[size_t(y + halo) * fw.w + x + halo]),
                    "A " + c.name + " tile differs from the full frame at " +
                        std::to_string(sx) + "," + std::to_string(sy));
          }
      }
  }
}

// The FX on rasters of each depth
class Source final : public TStandardRasterFx {
  Image m_image;

public:
  explicit Source(const Image &image) : m_image(image) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "composite-source-" + std::to_string(m_image.w);
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

template <class Pixel, class Fx>
void checkTiles(Fx &fx, int w, int h, const std::string &name) {
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  auto render    = [&](TPointD pos, int tw, int th) {
    TRasterPT<Pixel> raster(tw, th);
    TTile tile(raster, pos);
    fx.doCompute(tile, 0, settings);
    return raster;
  };
  const TRasterPT<Pixel> whole = render(TPointD(0, 0), w, h);
  const TRasterPT<Pixel> part  = render(TPointD(17, 11), 15, 13);
  bool any                     = false;
  for (int y = 0; y < 13; ++y)
    for (int x = 0; x < 15; ++x) {
      require(part->pixels(y)[x] == whole->pixels(y + 11)[x + 17],
              "A " + name + " tile differs from the whole frame");
      any = any || whole->pixels(y + 11)[x + 17].m != 0;
    }
  require(any, "The " + name + " render is empty");
}

template <class Pixel>
void testFx() {
  const Image fg = disk(56, 50, 18), bg(56, 50, Color{0.2, 0.5, 0.9, 1});
  TFxP fore = new Source(fg), back = new Source(bg);
  TFxP wrap = new LightWrapFx, extend = new EdgeExtendFx,
       choke = new MatteChokeFx, unmultiply = new UnmultiplyFx;
  require(wrap->connect("Source", fore.getPointer()) &&
              wrap->connect("Background", back.getPointer()) &&
              extend->connect("Source", fore.getPointer()) &&
              choke->connect("Source", fore.getPointer()) &&
              unmultiply->connect("Source", fore.getPointer()),
          "Cannot connect the sources");
  checkTiles<Pixel>(static_cast<LightWrapFx &>(*wrap), 56, 50, "light wrap");
  checkTiles<Pixel>(static_cast<EdgeExtendFx &>(*extend), 56, 50,
                    "edge extend");
  checkTiles<Pixel>(static_cast<MatteChokeFx &>(*choke), 56, 50, "choke");
  // Over Background covers the frame with the background
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  TRasterPT<Pixel> raster(8, 8);
  TTile corner(raster, TPointD(0, 0));
  static_cast<LightWrapFx &>(*wrap).doCompute(corner, 0, settings);
  const TPixelD c = PixelConverter<TPixelD>::from(raster->pixels(0)[0]);
  require(approx(c.m, 1, 1e-4) && approx(c.b, 0.9, 2e-3),
          "Light wrap did not composite over the background");
  // Unmultiply works at every depth (it crashed on 16-bit and float tiles)
  TRasterPT<Pixel> plain(56, 50);
  TTile tile(plain, TPointD(0, 0));
  static_cast<UnmultiplyFx &>(*unmultiply).doCompute(tile, 0, settings);
  // A pixel on the disk's antialiased edge, about 10% covered
  const TPixelD edge = PixelConverter<TPixelD>::from(plain->pixels(38)[40]);
  require(edge.m > 0.05 && edge.m < 0.95 && approx(edge.r, 0.3, 0.02),
          "Unmultiply did not divide the colour by its alpha");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testLightWrap();
    testEdgeExtend();
    testChoke();
    testTiling();
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: light wrap, edge extend, choke and spread, tiling, "
                 "unmultiply and pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
