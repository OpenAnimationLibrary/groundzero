#include "../celedgelinesfx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using CelEdge::Color;
using CelEdge::Kind;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}

const Color red{1, 0, 0, 1}, green{0, 1, 0, 1}, blue{0, 0, 1, 1},
    clear{0, 0, 0, 0};

struct Image {
  int w, h;
  std::vector<Color> pixels;
  Image(int w_, int h_, Color c = clear)
      : w(w_), h(h_), pixels(size_t(w_) * h_, c) {}
  Color &at(int x, int y) { return pixels[size_t(y) * w + x]; }
};

CelEdge::Rule rule(Kind a, Kind b, double weight, Color line = {0, 0, 0, 1}) {
  CelEdge::Rule r;
  r.enabled     = true;
  r.first.kind  = a;
  r.second.kind = b;
  r.weight      = weight;
  r.line        = line;
  return r;
}
CelEdge::Rule colorRule(const Color &a, const Color &b, double weight) {
  CelEdge::Rule r = rule(Kind::Color, Kind::Color, weight);
  r.first.r = a.r, r.first.g = a.g, r.first.b = a.b;
  r.second.r = b.r, r.second.g = b.g, r.second.b = b.b;
  return r;
}
std::vector<Color> linesOf(const Image &image,
                           const std::vector<CelEdge::Rule> &rules,
                           CelEdge::Settings settings = CelEdge::Settings()) {
  std::vector<Color> out;
  require(CelEdge::lines(image.pixels, image.w, image.h, settings, rules, out),
          "Lines were canceled");
  return out;
}
double rowInk(const std::vector<Color> &lines, int w, int y, int x0 = 0,
              int x1 = -1) {
  if (x1 < 0) x1 = w;
  double sum = 0;
  for (int x = x0; x < x1; ++x) sum += lines[size_t(y) * w + x].a;
  return sum;
}

// Red on the left, green on the right, optionally with an antialiased
// column between them
Image split(int w, int h, bool ramp) {
  Image image(w, h, red);
  for (int y = 0; y < h; ++y)
    for (int x = w / 2; x < w; ++x) image.at(x, y) = green;
  if (ramp)
    for (int y = 0; y < h; ++y) image.at(w / 2, y) = Color{0.5, 0.5, 0, 1};
  return image;
}

void testWeight() {
  for (bool ramp : {false, true})
    for (double weight : {1.0, 2.0, 2.5, 3.0, 6.0}) {
      const Image image = split(24, 8, ramp);
      const auto lines  = linesOf(image, {rule(Kind::Any, Kind::Any, weight)});
      for (int y = 0; y < image.h; ++y)
        require(near(rowInk(lines, image.w, y), weight, 1e-6),
                "A line of weight " + std::to_string(weight) +
                    " is not that wide" + (ramp ? " across a ramp" : ""));
      // Centred on the boundary: as much ink on each side
      const int middle = image.w / 2;
      if (!ramp)
        require(near(rowInk(lines, image.w, 3, 0, middle),
                     rowInk(lines, image.w, 3, middle), 1e-9),
                "A line is not centred on its boundary");
    }
}

void testRules() {
  // Red | green | blue stripes; only red-green gets a line
  Image image(30, 6, red);
  for (int y = 0; y < 6; ++y)
    for (int x = 10; x < 30; ++x) image.at(x, y) = x < 20 ? green : blue;
  const auto lines = linesOf(image, {colorRule(red, green, 2)});
  require(near(rowInk(lines, 30, 2, 0, 15), 2.0),
          "The red-green boundary has no line");
  require(near(rowInk(lines, 30, 2, 15, 30), 0.0),
          "The green-blue boundary has a line it has no rule for");
  // The order of a rule's ends does not matter
  const auto swapped = linesOf(image, {colorRule(green, red, 2)});
  require(near(rowInk(swapped, 30, 2, 0, 15), 2.0),
          "A rule did not match with its ends swapped");
  // The first matching rule wins
  const auto first =
      linesOf(image, {colorRule(red, green, 4), rule(Kind::Any, Kind::Any, 1)});
  require(near(rowInk(first, 30, 2, 0, 15), 4.0) &&
              near(rowInk(first, 30, 2, 15, 30), 1.0),
          "Rules were not applied in order");
  // A disabled rule draws nothing
  CelEdge::Rule off = rule(Kind::Any, Kind::Any, 2);
  off.enabled       = false;
  require(near(rowInk(linesOf(image, {off}), 30, 2), 0.0),
          "A disabled rule drew a line");
}

void testTransparent() {
  Image image(20, 20);
  for (int y = 6; y < 14; ++y)
    for (int x = 6; x < 14; ++x) image.at(x, y) = red;
  require(near(rowInk(linesOf(image, {rule(Kind::Any, Kind::Any, 2)}), 20, 10),
               0.0),
          "Any colour matched transparency");
  const auto outline = linesOf(image, {rule(Kind::Any, Kind::Transparent, 2)});
  require(near(rowInk(outline, 20, 10), 4.0),
          "The drawing's outline is not drawn on both sides");
  require(outline[size_t(10) * 20 + 5].a > 0.99 &&
              outline[size_t(10) * 20 + 4].a < 1e-9,
          "The outline does not reach half its weight outside");
}

void testFlatRegions() {
  // Noise within the tolerance is one region: no lines inside it
  Image image(32, 32, red);
  unsigned seed = 7;
  for (Color &c : image.pixels) {
    seed = seed * 1103515245u + 12345u;
    c.g  = (seed >> 16 & 255) / 255.0 * 0.02;
  }
  require(near(rowInk(linesOf(image, {rule(Kind::Any, Kind::Any, 2)}), 32, 16),
               0.0),
          "Noise within the tolerance made lines");
  // Beyond the gap, an unflat area is no region and gets no line
  Image grad(40, 4, red);
  for (int y = 0; y < 4; ++y)
    for (int x = 20; x < 40; ++x)
      grad.at(x, y) = Color{0, (x - 19) / 21.0, 0, 1};
  CelEdge::Settings settings;
  settings.tolerance = 0.01;
  const auto lines   = linesOf(grad, {rule(Kind::Any, Kind::Any, 2)}, settings);
  require(near(rowInk(lines, 40, 2, 24, 40), 0.0),
          "A gradient was cut into regions");
}

Image shapes(int w, int h) {
  Image image(w, h, clear);
  auto disk = [&](double cx, double cy, double r, Color c) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const double d     = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
        const double cover = std::max(0.0, std::min(1.0, r - d + 0.5));
        if (cover <= 0) continue;
        Color &p          = image.at(x, y);
        const double keep = 1 - cover;
        p = Color{c.r * cover + p.r * keep, c.g * cover + p.g * keep,
                  c.b * cover + p.b * keep, cover + p.a * keep};
      }
  };
  disk(30, 30, 22, red);
  disk(48, 36, 18, green);
  disk(40, 52, 12, blue);
  return image;
}

void testTiling() {
  // Every tiling gives the same pixels: a window with the halo around it
  // reproduces the full frame exactly
  const Image image = shapes(80, 72);
  CelEdge::Settings settings;
  const std::vector<CelEdge::Rule> rules = {
      rule(Kind::Any, Kind::Any, 3.0, {0, 0, 0, 1}),
      rule(Kind::Any, Kind::Transparent, 1.5, {0.2, 0.2, 0.8, 1})};
  std::vector<Color> full;
  require(
      CelEdge::render(image.pixels, image.w, image.h, settings, rules, full),
      "Full render canceled");
  const int halo = CelEdge::halo(settings, rules);
  double ink     = 0;
  for (const Color &c : full) ink += c.a;
  require(ink > 0, "The shapes got no lines");
  for (int x0 : {0, 13, 37})
    for (int y0 : {0, 21, 40}) {
      const int tw = 23, th = 19;
      Image window(tw + 2 * halo, th + 2 * halo);
      for (int y = 0; y < window.h; ++y)
        for (int x = 0; x < window.w; ++x) {
          const int sx = x0 - halo + x, sy = y0 - halo + y;
          if (sx >= 0 && sy >= 0 && sx < image.w && sy < image.h)
            window.at(x, y) = image.pixels[size_t(sy) * image.w + sx];
        }
      std::vector<Color> part;
      require(CelEdge::render(window.pixels, window.w, window.h, settings,
                              rules, part),
              "Window render canceled");
      for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
          const int sx = x0 + x, sy = y0 + y;
          if (sx >= image.w || sy >= image.h) continue;
          // Outside the image both see transparency, but the full frame
          // stops at its edge: compare only away from it
          if (sx < halo || sy < halo || sx >= image.w - halo ||
              sy >= image.h - halo)
            continue;
          const Color &a = full[size_t(sy) * image.w + sx];
          const Color &b = part[size_t(y + halo) * window.w + x + halo];
          require(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a,
                  "A tile differs from the full frame at " +
                      std::to_string(sx) + "," + std::to_string(sy));
        }
    }
}

void testCompositing() {
  const Image image = split(16, 4, false);
  CelEdge::Settings settings;
  settings.opacity                       = 0.5;
  const std::vector<CelEdge::Rule> rules = {
      rule(Kind::Any, Kind::Any, 2, {0, 0, 1, 1})};
  std::vector<Color> over, only;
  require(CelEdge::render(image.pixels, 16, 4, settings, rules, over),
          "Over render canceled");
  settings.linesOnly = true;
  require(CelEdge::render(image.pixels, 16, 4, settings, rules, only),
          "Lines-only render canceled");
  const Color &line = only[size_t(1) * 16 + 7];
  require(near(line.a, 0.5) && near(line.b, 0.5) && near(line.r, 0),
          "Opacity does not scale the premultiplied line");
  const Color &mixed = over[size_t(1) * 16 + 7];
  require(near(mixed.r, 0.5) && near(mixed.b, 0.5) && near(mixed.a, 1),
          "The line is not composited over the source");
  require(near(only[0].a, 0) && near(over[0].r, 1),
          "Pixels away from lines changed");
}

void testCancel() {
  const Image image = split(16, 4, false);
  int canceled      = 1;
  std::vector<Color> out(3);
  require(!CelEdge::render(image.pixels, 16, 4, CelEdge::Settings(),
                           {rule(Kind::Any, Kind::Any, 2)}, out, &canceled) &&
              out.empty(),
          "Cancellation did not clear the result");
}

void testInvalid() {
  CelEdge::Settings settings;
  settings.gap = -1;
  std::vector<Color> out;
  bool threw = false;
  try {
    CelEdge::lines(split(8, 2, false).pixels, 8, 2, settings, {}, out);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  require(threw, "A negative gap was accepted");
}

// The FX on rasters of each depth: the same lines as the math, and nothing
// changed without a rule
class Source final : public TStandardRasterFx {
  Image m_image;

public:
  explicit Source(const Image &image) : m_image(image) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "edge-source";
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
        const Color &c = m_image.pixels[size_t(sy) * m_image.w + sx];
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
  const Image image = split(24, 6, false);
  TFxP fx           = new CelEdgeLinesFx;
  TFxP source       = new Source(image);
  require(fx->connect("Source", source.getPointer()), "Cannot connect source");
  auto &edge = static_cast<CelEdgeLinesFx &>(*fx);
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  auto render    = [&](TPointD pos, int w, int h) {
    TRasterPT<Pixel> raster(w, h);
    TTile tile(raster, pos);
    edge.doCompute(tile, 0, settings);
    return raster;
  };
  // The first default rule draws black lines of weight 2 between the
  // colours; the second, around the drawing, is checked separately
  numeric(*fx, "use002")->setDefaultValue(0);
  const TRasterPT<Pixel> whole = render(TPointD(0, 0), 24, 6);
  const double tolerance =
      std::is_same<Pixel, TPixel32>::value ? 1.0 / 255 : 1e-4;
  for (int x = 0; x < 24; ++x) {
    const TPixelD p   = PixelConverter<TPixelD>::from(whole->pixels(3)[x]);
    const bool onLine = x == 11 || x == 12;
    require(onLine ? near(p.r, 0, tolerance) && near(p.g, 0, tolerance) &&
                         near(p.m, 1, tolerance)
                   : near(p.m, 1, tolerance) && (p.r > 0.99 || p.g > 0.99),
            "The FX line is wrong at x=" + std::to_string(x));
  }
  // A tile at an offset gives the same pixels
  const TRasterPT<Pixel> part = render(TPointD(9, 2), 6, 3);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 6; ++x)
      require(part->pixels(y)[x] == whole->pixels(y + 2)[x + 9],
              "An FX tile differs from the whole frame");
  // Around the drawing, where it meets the transparency beyond its edges
  numeric(*fx, "use002")->setDefaultValue(1);
  const TRasterPT<Pixel> outlined = render(TPointD(0, 0), 24, 6);
  const TPixelD rim    = PixelConverter<TPixelD>::from(outlined->pixels(3)[0]);
  const TPixelD inside = PixelConverter<TPixelD>::from(outlined->pixels(3)[2]);
  require(
      near(rim.r, 0, tolerance) && near(rim.m, 1, tolerance) && inside.r > 0.99,
      "The default outline around the drawing is missing");
  // Without any rule the source passes through unchanged
  numeric(*fx, "enabledRules")->setDefaultValue(0);
  const TRasterPT<Pixel> plain = render(TPointD(0, 0), 24, 6);
  for (int x = 0; x < 24; ++x) {
    const TPixelD p = PixelConverter<TPixelD>::from(plain->pixels(3)[x]);
    require(near(p.m, 1, tolerance) && (p.r > 0.99) == (x < 12),
            "Without rules the source changed");
  }
  // The alias changes with every rule value, so caches see new settings
  numeric(*fx, "enabledRules")->setDefaultValue(2);
  const std::string before = edge.getAlias(0, settings);
  numeric(*fx, "weight002")->setDefaultValue(5);
  require(edge.getAlias(0, settings) != before,
          "The alias ignores the line weight");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testWeight();
    testRules();
    testTransparent();
    testFlatRegions();
    testTiling();
    testCompositing();
    testCancel();
    testInvalid();
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: edge line weights, rules, outlines, regions, tiling, "
                 "compositing and pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
