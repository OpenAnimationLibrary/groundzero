#include "glbrenderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace otglb;
namespace {
void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance = 1e-5) {
  check(std::isfinite(actual) && std::abs(actual - expected) < tolerance,
        "Procedural renderer differs from its analytic result");
}
Matrix identity() {
  Matrix matrix{};
  matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
  return matrix;
}
Primitive plane(float z = 0, int material = NoIndex) {
  Primitive primitive;
  primitive.vertexCount = 4;
  primitive.triangleCount = 2;
  primitive.material = material;
  primitive.attributes = {
      {"POSITION", 3, {-1,-1,z, 1,-1,z, 1,1,z, -1,1,z}},
      {"NORMAL", 3, {0,0,1, 0,0,1, 0,0,1, 0,0,1}},
      {"TEXCOORD_0", 2, {0,0, 1,0, 1,1, 0,1}}};
  primitive.indices = {0,1,2, 0,2,3};
  return primitive;
}
Asset asset(std::vector<Primitive> primitives = {plane()}) {
  Asset result;
  result.meshes.push_back({"", std::move(primitives), {}});
  Node node;
  node.mesh = 0;
  node.local = node.world = identity();
  result.nodes.push_back(node);
  result.scenes.push_back({"", {0}, {}});
  result.defaultScene = 0;
  return result;
}
RenderTile tile(int size = 40) {
  RenderTile result;
  result.width = result.height = size;
  result.affine = {{.1, 0, double(size) / 2, 0, .1, double(size) / 2}};
  return result;
}
using Displace = std::function<double(const SurfaceSample &)>;
using Shade = std::function<SurfaceColor(const SurfaceSample &)>;
SurfaceColor unchanged(const SurfaceSample &sample) {
  return {{{sample.baseColor[0], sample.baseColor[1], sample.baseColor[2]}}, sample.baseColor[3]};
}
class Evaluator final : public SurfaceEvaluator {
  Displace displacement;
  Shade shading;
  std::thread::id owner = std::this_thread::get_id();

public:
  Evaluator(Displace displacement, Shade shading)
      : displacement(std::move(displacement)), shading(std::move(shading)) {}
  double displace(const SurfaceSample &sample) override {
    check(owner == std::this_thread::get_id(), "Geometry workers shared a mutable evaluator");
    return displacement(sample);
  }
  SurfaceColor shade(const SurfaceSample &sample) override {
    check(owner == std::this_thread::get_id(), "Tile workers shared a mutable evaluator");
    return shading(sample);
  }
};
std::shared_ptr<const SurfaceRequest> request(
    Displace displace = [](const SurfaceSample &) { return 0.0; },
    Shade shade = unchanged) {
  auto result = std::make_shared<SurfaceRequest>();
  result->identity = "fixture-v1";
  result->frame = 2;
  result->time = .5;
  result->createEvaluator = [=](const int *) {
    return std::make_unique<Evaluator>(displace, shade);
  };
  return result;
}
void equal(const std::vector<ColorPixel> &actual, const std::vector<ColorPixel> &expected) {
  check(actual.size() == expected.size(), "Procedural image size changed");
  for (std::size_t i = 0; i < actual.size(); ++i) {
    near(actual[i].r, expected[i].r);
    near(actual[i].g, expected[i].g);
    near(actual[i].b, expected[i].b);
    near(actual[i].alpha, expected[i].alpha);
  }
}
template <class F> void rejects(F operation) {
  bool rejected = false;
  try { operation(); } catch (const std::runtime_error &) { rejected = true; }
  check(rejected, "Invalid procedural render did not fail explicitly");
}

void geometryAndNormals() {
  auto input = asset();
  input.nodes[0].world[12] = 3;
  RenderOptions options;
  options.position = {{1, 0, 2}};
  options.cameraDistance = 20;
  ModelTransform scale;
  scale.scale = {{2, 3, 4}};
  options.transforms.push_back(scale);
  options.surface = request([](const SurfaceSample &sample) {
    near(sample.objectNormal[0], 0); near(sample.objectNormal[2], 1);
    near(sample.worldPosition[0], 2 * (sample.objectPosition[0] + 4));
    near(sample.worldPosition[2], 8);
    near(sample.cameraPosition[2], -12);
    near(sample.frame, 2); near(sample.time, .5);
    return .5 * sample.objectPosition[0];
  });
  const auto scene = prepareRender(input, options);
  check(scene.triangles.size() == 2 && scene.surface == options.surface,
        "Displacement lost the surface request or plane topology");
  for (const auto &triangle : scene.triangles) {
    check(bool(triangle.surface), "Displacement lost its interpolation payload");
    for (const auto &vertex : triangle.surface->vertices) {
      near(vertex.objectPosition[2], .5 * vertex.objectPosition[0]);
      near(vertex.worldPosition[0], 2 * (vertex.objectPosition[0] + 4));
      near(vertex.worldPosition[2], 4 * (vertex.objectPosition[2] + 2));
      near(vertex.cameraPosition[2], vertex.worldPosition[2] - 20);
      near(vertex.objectNormal[0], -.5 / std::sqrt(1.25));
      near(vertex.objectNormal[2], 1 / std::sqrt(1.25));
      near(vertex.worldNormal[0], -1 / std::sqrt(2.0));
      near(vertex.worldNormal[2], 1 / std::sqrt(2.0));
    }
  }
  near(scene.bounds[0], 600); near(scene.bounds[2], 1000);
  auto other = options;
  auto changed = std::make_shared<SurfaceRequest>(*options.surface);
  changed->identity += "-edit";
  other.surface = changed;
  check(!(options == other), "Surface source identity is absent from the projection cache key");
  changed->identity = options.surface->identity;
  changed->frame += 1;
  check(!(options == other), "Surface frame is absent from the projection cache key");
}

void blendedSkinNormals() {
  auto input = asset();
  auto &primitive = input.meshes[0].primitives[0];
  primitive.attributes[0].values = {-1,1,-1, -1,1,1, 1,-1,1, 1,-1,-1};
  const float component = float(1 / std::sqrt(2.0));
  primitive.attributes[1].values = {component,component,0, component,component,0,
                                    component,component,0, component,component,0};
  primitive.attributes.push_back({"JOINTS_0", 4,
      {0,1,0,0, 0,1,0,0, 0,1,0,0, 0,1,0,0}});
  primitive.attributes.push_back({"WEIGHTS_0", 4,
      {.5f,.5f,0,0, .5f,.5f,0,0, .5f,.5f,0,0, .5f,.5f,0,0}});
  input.nodes[0].hasSkin = true; input.nodes[0].skin = 0;
  Node first, second;
  first.local = first.world = second.local = second.world = identity();
  first.scale = {{2, 1, 1}};
  input.nodes.push_back(first); input.nodes.push_back(second);
  input.scenes[0].roots = {0, 1, 2};
  Skin skin; skin.joints = {1, 2}; skin.inverseBindMatrices = {identity(), identity()};
  input.skins.push_back(skin);
  input.animations.push_back(Animation{});
  RenderOptions options;
  options.animation = 0;
  options.rotation = {{45, 0, 0}};
  const double length = std::hypot(2.0 / 3, 1.0);
  options.surface = request([=](const SurfaceSample &sample) {
    near(sample.objectNormal[0], (2.0 / 3) / length);
    near(sample.objectNormal[1], 1 / length);
    near(sample.objectNormal[2], 0);
    return 0.0;
  });
  const auto scene = prepareRender(input, options);
  check(scene.triangles.size() == 2, "Mixed-scale skin lost its procedural surface");
  for (const auto &triangle : scene.triangles) for (const auto &vertex : triangle.surface->vertices) {
    near(vertex.objectNormal[0], (2.0 / 3) / length);
    near(vertex.objectNormal[1], 1 / length);
  }
}

RenderScene gradientScene() {
  RenderOptions options;
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &sample) {
        near(sample.frame, 2); near(sample.time, .5);
        near(sample.uv[0], (sample.objectPosition[0] + 1) / 2);
        near(sample.uv[1], (sample.objectPosition[1] + 1) / 2);
        return SurfaceColor{{{.5 + .25 * sample.objectPosition[0], sample.uv[1],
                                (sample.frame + sample.time) / 10}}, 1};
      });
  return prepareRender(asset(), options);
}

void actualSampleShadingAndWorkers() {
  const auto scene = gradientScene();
  const auto image = renderTile(scene, tile());
  const auto &center = image[20 * 40 + 20];
  near(center.r, .5125); near(center.g, .525); near(center.b, .25); near(center.alpha, 1);
  near(image[20 * 40 + 16].r, .4125);
  near(image[20 * 40 + 24].r, .6125);
  auto first = std::async(std::launch::async, [&] { return renderTile(scene, tile()); });
  auto second = std::async(std::launch::async, [&] { return renderTile(scene, tile()); });
  equal(first.get(), image); equal(second.get(), image);
  for (int origin : {0, 20}) {
    auto part = tile();
    part.width = 20;
    part.x = origin;
    const auto split = renderTile(scene, part);
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 20; ++x) {
      const auto &actual = split[y * 20 + x], &expected = image[y * 40 + x + origin];
      near(actual.r, expected.r); near(actual.g, expected.g);
      near(actual.b, expected.b); near(actual.alpha, expected.alpha);
    }
  }
}

void transparentOverlap() {
  auto input = asset({plane(0, 0), plane(-1, 1)});
  Material front, back;
  front.baseColor = {{1, 0, 0, 1}};
  back.baseColor = {{0, 0, 1, 1}};
  input.materials = {front, back};
  RenderOptions options;
  options.materialColors = true;
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &sample) {
        auto color = unchanged(sample); color.opacity = .5; return color;
      });
  const auto image = renderTile(prepareRender(input, options), tile());
  const auto center = image[20 * 40 + 20];
  near(center.r, .5); near(center.g, 0); near(center.b, .25); near(center.alpha, .75);
  // Two subpixel samples are on the quad's shared diagonal. Blending both
  // adjacent triangles there would produce the wrong color and opacity.
  for (int i = 12; i < 28; ++i) {
    near(image[i * 40 + i].r, .5);
    near(image[i * 40 + i].alpha, .75);
  }
  std::reverse(input.meshes[0].primitives.begin(), input.meshes[0].primitives.end());
  equal(renderTile(prepareRender(input, options), tile()), image);
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &sample) {
        auto color = unchanged(sample);
        color.opacity = sample.objectPosition[2] < -.5 ? 1 : .5;
        return color;
      });
  const auto opaqueBack = renderTile(prepareRender(input, options), tile())[20 * 40 + 20];
  near(opaqueBack.r, .5); near(opaqueBack.b, .5); near(opaqueBack.alpha, 1);
}

void coloredOpacity() {
  auto input = asset({plane(0), plane(-1)});
  RenderOptions options;
  const auto shade = [](const SurfaceSample &sample) {
    if (sample.objectPosition[2] > -.5)
      return SurfaceColor{{{1, .5, .25}}, .5, {{.5, 1, 0}}};
    if (sample.objectPosition[2] > -1.5)
      return SurfaceColor{{{0, .25, 1}}, .8, {{1, .25, .5}}};
    return SurfaceColor{{{.2, .4, .6}}, 1};
  };
  options.surface = request([](const SurfaceSample &) { return 0.0; }, shade);
  const auto scene = prepareRender(input, options);
  const auto image = renderTile(scene, tile());
  // Front Oi=(.25,.5,0), back Oi=(.8,.2,.4): transmission is
  // (.15,.4,.6), and premultiplied RGB is (.25,.275,.4).
  const auto &center = image[20 * 40 + 20];
  near(center.r, .25); near(center.g, .275); near(center.b, .4); near(center.alpha, .85);
  for (int i = 12; i < 28; ++i) {
    const auto &diagonal = image[i * 40 + i];
    near(diagonal.r, .25); near(diagonal.g, .275);
    near(diagonal.b, .4); near(diagonal.alpha, .85);
  }
  std::reverse(input.meshes[0].primitives.begin(), input.meshes[0].primitives.end());
  equal(renderTile(prepareRender(input, options), tile()), image);
  auto first = std::async(std::launch::async, [&] { return renderTile(scene, tile()); });
  auto second = std::async(std::launch::async, [&] { return renderTile(scene, tile()); });
  equal(first.get(), image); equal(second.get(), image);
  for (int origin : {0, 20}) {
    auto part = tile(); part.width = 20; part.x = origin;
    const auto split = renderTile(scene, part);
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 20; ++x) {
      const auto &actual = split[y * 20 + x], &expected = image[y * 40 + x + origin];
      near(actual.r, expected.r); near(actual.g, expected.g);
      near(actual.b, expected.b); near(actual.alpha, expected.alpha);
    }
  }
  input.meshes[0].primitives.push_back(plane(-2));
  const auto opaqueBack = renderTile(prepareRender(input, options), tile())[20 * 40 + 20];
  // An opaque third layer contributes (.15,.4,.6)*(.2,.4,.6).
  near(opaqueBack.r, .28); near(opaqueBack.g, .435);
  near(opaqueBack.b, .76); near(opaqueBack.alpha, 1);

  auto single = asset();
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &) { return SurfaceColor{{{.5, .25, .75}}, .5, {{2, .5, -2}}}; });
  const auto clamped = renderTile(prepareRender(single, options), tile())[20 * 40 + 20];
  // Clamp RGB Oi before multiplying scalar alpha: effective Oi=(.5,.25,0).
  near(clamped.r, .25); near(clamped.g, .0625); near(clamped.b, 0); near(clamped.alpha, .5);
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &) { return SurfaceColor{{{.5, .25, .75}}, 2, {{-2, .5, 3}}}; });
  const auto scalarClamped = renderTile(prepareRender(single, options), tile())[20 * 40 + 20];
  near(scalarClamped.r, 0); near(scalarClamped.g, .125);
  near(scalarClamped.b, .75); near(scalarClamped.alpha, 1);
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &sample) {
        const bool red = sample.objectPosition[0] > .05;
        return SurfaceColor{{{1, 1, 1}}, 1, {{red ? 1.0 : 0.0, red ? 0.0 : 1.0, 0}}};
      });
  const auto subpixel = renderTile(prepareRender(single, options), tile())[20 * 40 + 20];
  // Two red-only and two green-only samples each have full coverage. Alpha is
  // averaged after per-sample flattening, rather than max of averaged RGB Oi.
  near(subpixel.r, .5); near(subpixel.g, .5); near(subpixel.b, 0); near(subpixel.alpha, 1);

  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()}) {
    for (int channel = 0; channel < 3; ++channel) {
      options.surface = request([](const SurfaceSample &) { return 0.0; },
          [=](const SurfaceSample &) {
            SurfaceColor result; result.coloredOpacity[channel] = invalid; return result;
          });
      const auto invalidScene = prepareRender(single, options);
      rejects([&] { renderTile(invalidScene, tile()); });
    }
  }
  // Validation must not hide malformed RGB opacity behind zero scalar alpha.
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &) {
        return SurfaceColor{{{1, 1, 1}}, 0,
                            {{std::numeric_limits<double>::quiet_NaN(), 1, 1}}};
      });
  const auto hiddenInvalid = prepareRender(single, options);
  rejects([&] { renderTile(hiddenInvalid, tile()); });

  // A uniform RGB multiplier retains the scalar path's exact pixel rounding.
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &) { return SurfaceColor{{{.3, .7, .9}}, .3}; });
  const auto scalar = renderTile(prepareRender(input, options), tile());
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &) { return SurfaceColor{{{.3, .7, .9}}, .6, {{.5, .5, .5}}}; });
  const auto uniform = renderTile(prepareRender(input, options), tile());
  check(scalar.size() == uniform.size(), "Uniform RGB opacity changed the tile shape");
  for (std::size_t i = 0; i < scalar.size(); ++i)
    check(scalar[i].r == uniform[i].r && scalar[i].g == uniform[i].g &&
          scalar[i].b == uniform[i].b && scalar[i].alpha == uniform[i].alpha,
          "Uniform RGB opacity changed scalar composition rounding");
}

void perspectiveAndClipping() {
  auto input = asset();
  auto &primitive = input.meshes[0].primitives[0];
  primitive.vertexCount = 3; primitive.triangleCount = 1; primitive.indices.clear();
  primitive.attributes = {{"POSITION", 3, {-1,-1,0, 1,-1,0, 0,1,1.5f}},
                           {"TEXCOORD_0", 2, {0,0, 1,0, .5f,1}}};
  RenderOptions options;
  options.cameraDistance = 2; options.nearClip = 1;
  options.perspective = true; options.fieldOfView = 90;
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &sample) {
        near(sample.objectPosition[2], .75 * (sample.objectPosition[1] + 1));
        near(sample.cameraPosition[2], sample.objectPosition[2] - 2);
        near(sample.uv[0], (sample.objectPosition[0] + 1) / 2);
        return SurfaceColor{{{(sample.objectPosition[0] + 1) / 2,
                                (sample.objectPosition[1] + 1) / 2, sample.uv[0]}}, 1};
      });
  const auto scene = prepareRender(input, options);
  check(scene.triangles.size() == 2, "Near-plane clipping did not retain the visible surface polygon");
  for (const auto &triangle : scene.triangles) for (const auto &v : triangle.surface->vertices)
    check(v.cameraPosition[2] <= -1 + 1e-10, "Clipped shading point escaped the near plane");
  RenderTile rendering;
  rendering.width = rendering.height = 80;
  rendering.affine = {{.05, 0, 40, 0, .05, 40}};
  const auto image = renderTile(scene, rendering);
  double red = 0, green = 0;
  for (double y : {.25, .75}) for (double x : {.25, .75}) {
    const double qy = y / 25, qx = x / 25;
    const double objectY = 1.25 * qy / (1 + .75 * qy);
    const double distance = 1.25 - .75 * objectY;
    red += (.5 + .5 * qx * distance) / 4;
    green += (.5 + .5 * objectY) / 4;
  }
  const auto &center = image[40 * 80 + 40];
  near(center.r, red); near(center.g, green); near(center.b, red); near(center.alpha, 1);
}

void imagePlaneContextAndBounds() {
  auto shadeCalls = std::make_shared<std::atomic<int>>(0);
  auto surface = request([](const SurfaceSample &sample) {
    near(sample.baseColor[3], 1); return .1;
  }, [shadeCalls](const SurfaceSample &sample) {
    ++*shadeCalls; return unchanged(sample);
  });
  const auto geometry = projectImagePlane(2, 2, {}, surface);
  check(shadeCalls->load() == 0 && !geometry.texture,
        "Image-plane bounds evaluated source pixels or invoked the shader");
  for (const auto &triangle : geometry.triangles) for (const auto &v : triangle.surface->vertices)
    near(v.objectPosition[2], .1);
  auto pixels = std::make_shared<std::vector<ColorPixel>>(4, ColorPixel{.125f,.25f,.375f,.5f});
  const auto scene = prepareImagePlane(2, 2, pixels, {}, surface);
  RenderTile rendering; rendering.width = rendering.height = 4; rendering.x = rendering.y = -2;
  const auto image = renderTile(scene, rendering);
  const auto &center = image[1 * 4 + 1];
  near(center.r, .125); near(center.g, .25); near(center.b, .375); near(center.alpha, .5);
  near(scene.bounds[0], geometry.bounds[0]); near(scene.bounds[2], geometry.bounds[2]);
  check(shadeCalls->load() > 0, "Image-plane rasterization did not invoke its actual shader");
}

void failuresCancellationAndCompatibility() {
  auto input = asset();
  RenderOptions options;
  const auto original = renderTile(prepareRender(input, options), tile());
  options.surface = request();
  options.surface.reset();
  equal(renderTile(prepareRender(input, options), tile()), original);
  near(original[20 * 40 + 20].r, .75);
  options.surface = request([](const SurfaceSample &) { return std::numeric_limits<double>::quiet_NaN(); });
  rejects([&] { prepareRender(input, options); });
  options.surface = request([](const SurfaceSample &) { throw std::runtime_error("fixture displacement failed"); return 0.0; });
  rejects([&] { prepareRender(input, options); });
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &) { return SurfaceColor{{{1, 0, 0}}, std::numeric_limits<double>::quiet_NaN()}; });
  const auto badScene = prepareRender(input, options);
  rejects([&] { renderTile(badScene, tile()); });
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [](const SurfaceSample &) -> SurfaceColor { throw std::runtime_error("fixture shader failed"); });
  const auto throwingScene = prepareRender(input, options);
  rejects([&] { renderTile(throwingScene, tile()); });
  auto invalid = std::make_shared<SurfaceRequest>();
  options.surface = invalid;
  rejects([&] { prepareRender(input, options); });
  auto noWorker = std::make_shared<SurfaceRequest>(*request());
  noWorker->createEvaluator = [](const int *) { return std::unique_ptr<SurfaceEvaluator>(); };
  options.surface = noWorker;
  rejects([&] { prepareRender(input, options); });
  int canceled = 1;
  options.surface = request();
  check(prepareRender(input, options, &canceled).triangles.empty(), "Surface preparation ignored cancellation");
  const auto scene = prepareRender(input, options);
  check(renderTile(scene, tile(), &canceled).empty(), "Surface rasterization ignored cancellation");
  canceled = 0;
  auto calls = std::make_shared<int>(0);
  options.surface = request([](const SurfaceSample &) { return 0.0; },
      [calls, &canceled](const SurfaceSample &sample) {
        if (++*calls == 16) canceled = 1;
        return unchanged(sample);
      });
  const auto cancelScene = prepareRender(input, options);
  check(renderTile(cancelScene, tile(), &canceled).empty() && canceled,
        "Cancellation during real shading returned a successful partial image");
  auto huge = tile(); huge.width = huge.height = 100000;
  rejects([&] { renderTile(scene, huge); });
}
}  // namespace

int main() {
  try {
    geometryAndNormals();
    blendedSkinNormals();
    actualSampleShadingAndWorkers();
    transparentOverlap();
    coloredOpacity();
    perspectiveAndClipping();
    imagePlaneContextAndBounds();
    failuresCancellationAndCompatibility();
    std::cout << "GLB procedural surface: PASS geometry, normals, contexts, samples, opacity, workers, cache identity and failures\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "GLB procedural surface: FAIL: " << error.what() << '\n';
    return 1;
  }
}
