#include "glbrenderer.h"
#include "glbpose.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace otglb {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr std::size_t MemoryLimit = 256ull * 1024 * 1024;
struct Vec { double x, y, z; };
Vec add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec scaled(Vec a, double factor) { return {a.x * factor, a.y * factor, a.z * factor}; }
Vec vector(const std::array<double, 3> &a) { return {a[0], a[1], a[2]}; }
std::array<double, 3> array(Vec a) { return {{a.x, a.y, a.z}}; }
Vec sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec cross(Vec a, Vec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
Vec normalized(Vec value) {
  const double length = std::hypot(value.x, value.y, value.z);
  require(std::isfinite(length), "Non-finite procedural surface normal.");
  return length > 1e-15 ? scaled(value, 1.0 / length) : Vec{0, 0, 0};
}
void finitePosition(Vec value) {
  for (double coordinate : array(value))
    require(std::isfinite(coordinate) && std::abs(coordinate) <= 1e12,
            "Procedural surface position exceeds the supported coordinate range.");
}
Vec transform(const Matrix &m, Vec p) {
  return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
          m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
          m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}
Vec transformNormal(const Matrix &m, Vec n) {
  const Vec a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
  const double determinant = dot(a, cross(b, c));
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-30) return {0, 0, 0};
  return normalized(scaled(add(add(scaled(cross(b, c), n.x),
                                   scaled(cross(c, a), n.y)),
                               scaled(cross(a, b), n.z)), 1.0 / determinant));
}
Vec transform(Vec p, const ModelTransform &t) {
  const double sx = std::sin(t.rotation[0] * Pi / 180.0);
  const double cx = std::cos(t.rotation[0] * Pi / 180.0);
  const double sy = std::sin(t.rotation[1] * Pi / 180.0);
  const double cy = std::cos(t.rotation[1] * Pi / 180.0);
  const double sz = std::sin(t.rotation[2] * Pi / 180.0);
  const double cz = std::cos(t.rotation[2] * Pi / 180.0);
  p               = {p.x * t.scale[0], p.y * t.scale[1], p.z * t.scale[2]};
  p               = {p.x, cx * p.y - sx * p.z, sx * p.y + cx * p.z};
  p               = {cy * p.x + sy * p.z, p.y, -sy * p.x + cy * p.z};
  p               = {cz * p.x - sz * p.y, sz * p.x + cz * p.y, p.z};
  return {p.x + t.position[0], p.y + t.position[1], p.z + t.position[2]};
}
struct Instance {
  const RenderOptions &o;
  double sx, cx, sy, cy, sz, cz;
  explicit Instance(const RenderOptions &options) : o(options) {
    sx = std::sin(o.rotation[0] * Pi / 180); cx = std::cos(o.rotation[0] * Pi / 180);
    sy = std::sin(o.rotation[1] * Pi / 180); cy = std::cos(o.rotation[1] * Pi / 180);
    sz = std::sin(o.rotation[2] * Pi / 180); cz = std::cos(o.rotation[2] * Pi / 180);
  }
  Vec operator()(Vec p) const {
    p = {p.x * o.scale, p.y * o.scale, p.z * o.scale};
    p = {p.x, cx * p.y - sx * p.z, sx * p.y + cx * p.z};
    p = {cy * p.x + sy * p.z, p.y, -sy * p.x + cy * p.z};
    p = {cz * p.x - sz * p.y, sz * p.x + cz * p.y, p.z};
    p = {p.x + o.position[0], p.y + o.position[1], p.z + o.position[2]};
    for (const auto &additional : o.transforms) p = transform(p, additional);
    return {p.x, p.y, p.z - o.cameraDistance};
  }
  Vec normal(Vec n) const {
    n = {n.x, cx * n.y - sx * n.z, sx * n.y + cx * n.z};
    n = {cy * n.x + sy * n.z, n.y, -sy * n.x + cy * n.z};
    n = {cz * n.x - sz * n.y, sz * n.x + cz * n.y, n.z};
    for (const auto &additional : o.transforms) {
      n = {n.x / additional.scale[0], n.y / additional.scale[1],
           n.z / additional.scale[2]};
      ModelTransform rotation = additional;
      rotation.position = {{0, 0, 0}};
      rotation.scale = {{1, 1, 1}};
      n = transform(n, rotation);
    }
    return normalized(n);
  }
};

struct InfluenceSet {
  const Attribute *joints = nullptr;
  const Attribute *weights = nullptr;
};

std::vector<InfluenceSet> influenceSets(const Primitive &primitive) {
  std::vector<InfluenceSet> sets;
  for (const auto &attribute : primitive.attributes) {
    if (attribute.semantic.rfind("JOINTS_", 0) != 0) continue;
    const std::string weightsName = "WEIGHTS_" + attribute.semantic.substr(7);
    const auto weights = std::find_if(primitive.attributes.begin(), primitive.attributes.end(),
        [&](const Attribute &candidate) { return candidate.semantic == weightsName; });
    require(weights != primitive.attributes.end(),
            "GLB skinned primitive has JOINTS without matching WEIGHTS.");
    require(attribute.components == 4 && weights->components == 4 &&
                attribute.values.size() >= primitive.vertexCount * 4 &&
                weights->values.size() >= primitive.vertexCount * 4,
            "GLB skin influence attribute shape is invalid.");
    sets.push_back({&attribute, &*weights});
  }
  return sets;
}

// Sutherland-Hodgman clipping in camera space, before perspective division.
std::vector<Vec> clip(const std::vector<Vec> &polygon, double distance, bool near) {
  std::vector<Vec> out;
  if (polygon.empty()) return out;
  Vec a = polygon.back();
  auto side = [=](Vec p) { return near ? -p.z - distance : distance + p.z; };
  double da = side(a);
  for (Vec b : polygon) {
    const double db = side(b);
    if ((da >= 0) != (db >= 0)) {
      const double t = da / (da - db);
      out.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y),
                     a.z + t * (b.z - a.z)});
    }
    if (db >= 0) out.push_back(b);
    a = b; da = db;
  }
  return out;
}
double edge(const ProjectedVertex &a, const ProjectedVertex &b, double x, double y) {
  return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

SurfaceVertex interpolate(const SurfaceVertex &a, const SurfaceVertex &b, double t) {
  SurfaceVertex result;
  const auto blend = [=](const auto &left, const auto &right, auto &out) {
    for (std::size_t component = 0; component < out.size(); ++component)
      out[component] = left[component] + t * (right[component] - left[component]);
  };
  blend(a.objectPosition, b.objectPosition, result.objectPosition);
  blend(a.worldPosition, b.worldPosition, result.worldPosition);
  blend(a.cameraPosition, b.cameraPosition, result.cameraPosition);
  blend(a.objectNormal, b.objectNormal, result.objectNormal);
  blend(a.worldNormal, b.worldNormal, result.worldNormal);
  blend(a.cameraNormal, b.cameraNormal, result.cameraNormal);
  blend(a.uv, b.uv, result.uv);
  return result;
}

std::vector<SurfaceVertex> clipSurface(const std::vector<SurfaceVertex> &polygon,
                                     double distance, bool near) {
  std::vector<SurfaceVertex> result;
  if (polygon.empty()) return result;
  auto a = polygon.back();
  const auto side = [=](const SurfaceVertex &v) {
    return near ? -v.cameraPosition[2] - distance : distance + v.cameraPosition[2];
  };
  double da = side(a);
  for (const auto &b : polygon) {
    const double db = side(b);
    if ((da >= 0) != (db >= 0)) result.push_back(interpolate(a, b, da / (da - db)));
    if (db >= 0) result.push_back(b);
    a = b;
    da = db;
  }
  return result;
}

SurfaceSample surfaceSample(const SurfaceVertex &vertex,
                            const std::array<double, 4> &base,
                            const SurfaceRequest &request) {
  SurfaceSample sample;
  sample.objectPosition = vertex.objectPosition;
  sample.worldPosition = vertex.worldPosition;
  sample.cameraPosition = vertex.cameraPosition;
  sample.objectNormal = array(normalized(vector(vertex.objectNormal)));
  sample.worldNormal = array(normalized(vector(vertex.worldNormal)));
  sample.cameraNormal = array(normalized(vector(vertex.cameraNormal)));
  sample.uv = vertex.uv;
  sample.baseColor = base;
  sample.frame = request.frame;
  sample.time = request.time;
  return sample;
}

void validateSurface(const std::shared_ptr<const SurfaceRequest> &request) {
  if (!request) return;
  require(!request->identity.empty() && request->identity.size() <= 1048576 &&
              bool(request->createEvaluator) && std::isfinite(request->frame) &&
              std::isfinite(request->time),
          "Procedural surface requires a bounded identity, worker factory and finite time.");
}

template <class Project>
bool appendSurfacePrimitive(RenderScene &out, const Primitive &primitive,
                            const Matrix &nodeWorld, const SkinPose *skinPose,
                            const RenderOptions &options, const Instance &instance,
                            const Project &project, const std::array<float, 3> &base,
                            SurfaceEvaluator &evaluator, const int *canceled) {
  const std::size_t count = primitive.indices.empty() ? primitive.vertexCount : primitive.indices.size();
  const std::size_t triangleCount = count < 3 ? 0 :
      primitive.mode == 4 ? count / 3 : count - 2;
  // Include indices, normal accumulation and old/new projected allocations.
  const std::size_t triangleBytes = sizeof(RenderTriangle) + sizeof(SurfaceTriangle);
  require(primitive.vertexCount <= MemoryLimit / (4 * sizeof(SurfaceVertex)) &&
              triangleCount <= MemoryLimit / (12 * sizeof(std::size_t)) &&
              out.triangles.size() <= MemoryLimit / (3 * triangleBytes),
          "Procedural surface geometry exceeds the 256 MiB render budget.");
  const auto vertexBytes = primitive.vertexCount * sizeof(SurfaceVertex);
  const auto indexBytes = triangleCount * sizeof(std::array<std::size_t, 3>);
  const auto existingBytes = out.triangles.size() * 3 * triangleBytes;
  require(vertexBytes + indexBytes <= MemoryLimit - existingBytes,
          "Procedural surface working geometry exceeds the 256 MiB render budget.");
  if (!triangleCount) return true;

  const auto attribute = [&](const char *name, int components) -> const Attribute * {
    const auto found = std::find_if(primitive.attributes.begin(), primitive.attributes.end(),
        [&](const Attribute &candidate) { return candidate.semantic == name; });
    if (found == primitive.attributes.end()) return nullptr;
    require(found->components == components &&
                found->values.size() >= primitive.vertexCount * std::size_t(components),
            "Procedural surface attribute shape is invalid.");
    return &*found;
  };
  const auto positions = attribute("POSITION", 3);
  const auto normals = attribute("NORMAL", 3);
  const auto uv = attribute("TEXCOORD_0", 2);
  require(positions != nullptr, "Procedural surface geometry has no positions.");
  const auto influences = skinPose ? influenceSets(primitive) : std::vector<InfluenceSet>();
  if (skinPose) require(!influences.empty(), "Procedural surface skin has no joint weights.");
  std::vector<SurfaceVertex> vertices(primitive.vertexCount);
  std::vector<std::array<std::size_t, 3>> triangles;
  triangles.reserve(triangleCount);
  const auto indexAt = [&](std::size_t index) {
    const auto result = primitive.indices.empty() ? index : std::size_t(primitive.indices.at(index));
    require(result < vertices.size(), "Procedural surface triangle index is out of range.");
    return result;
  };
  for (std::size_t i = 2; i < count; i += primitive.mode == 4 ? 3 : 1) {
    if ((triangles.size() & 255) == 0 && canceled && *canceled) return false;
    auto face = std::array<std::size_t, 3>{{indexAt(primitive.mode == 6 ? 0 : i - 2),
                                          indexAt(i - 1), indexAt(i)}};
    if (primitive.mode == 5 && (i & 1)) std::swap(face[0], face[1]);
    triangles.push_back(face);
  }
  const auto updatePosition = [&](SurfaceVertex &vertex) {
    const Vec camera = instance(transform(nodeWorld, vector(vertex.objectPosition)));
    const Vec world{camera.x, camera.y, camera.z + options.cameraDistance};
    finitePosition(camera);
    finitePosition(world);
    vertex.cameraPosition = array(camera);
    vertex.worldPosition = array(world);
  };
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    if ((i & 255) == 0 && canceled && *canceled) return false;
    Vec local{positions->values[i * 3], positions->values[i * 3 + 1], positions->values[i * 3 + 2]};
    if (skinPose) {
      Vec weighted{};
      double total = 0;
      for (const auto &set : influences) for (int component = 0; component < 4; ++component) {
        const std::size_t at = i * 4 + component;
        const double weight = set.weights->values[at];
        require(std::isfinite(weight) && weight >= 0, "Invalid procedural surface skin weight.");
        if (!weight) continue;
        const double rawJoint = set.joints->values[at];
        require(std::isfinite(rawJoint) && rawJoint >= 0 &&
                    rawJoint < double(skinPose->jointMatrices.size()) &&
                    std::abs(rawJoint - std::floor(rawJoint + .5)) < 1e-6,
                "Procedural surface skin references an invalid joint.");
        weighted = add(weighted, scaled(transform(skinPose->jointMatrices[std::size_t(rawJoint + .5)], local), weight));
        total += weight;
      }
      require(std::isfinite(total) && total > 1e-12, "Procedural surface skin has no positive weights.");
      local = scaled(weighted, 1.0 / total);
    }
    finitePosition(local);
    vertices[i].objectPosition = array(local);
    updatePosition(vertices[i]);
    if (uv) {
      vertices[i].uv = {{uv->values[i * 2], uv->values[i * 2 + 1]}};
      for (double value : vertices[i].uv) require(std::isfinite(value), "Non-finite procedural surface UV.");
    }
  }
  const auto recomputeNormals = [&] {
    for (std::size_t i = 0; i < vertices.size(); ++i) {
      if ((i & 255) == 0 && canceled && *canceled) return false;
      vertices[i].objectNormal = vertices[i].worldNormal = {{0, 0, 0}};
    }
    std::size_t ordinal = 0;
    for (const auto &face : triangles) {
      if ((ordinal++ & 255) == 0 && canceled && *canceled) return false;
      const auto &a = vertices[face[0]], &b = vertices[face[1]], &c = vertices[face[2]];
      const Vec object = cross(sub(vector(b.objectPosition), vector(a.objectPosition)),
                               sub(vector(c.objectPosition), vector(a.objectPosition)));
      const Vec world = cross(sub(vector(b.worldPosition), vector(a.worldPosition)),
                              sub(vector(c.worldPosition), vector(a.worldPosition)));
      for (auto index : face) {
        vertices[index].objectNormal = array(add(vector(vertices[index].objectNormal), object));
        vertices[index].worldNormal = array(add(vector(vertices[index].worldNormal), world));
      }
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
      if ((i & 255) == 0 && canceled && *canceled) return false;
      auto &vertex = vertices[i];
      vertex.objectNormal = array(normalized(vector(vertex.objectNormal)));
      vertex.worldNormal = array(normalized(vector(vertex.worldNormal)));
      vertex.cameraNormal = vertex.worldNormal;
    }
    return true;
  };
  if (!recomputeNormals()) return false;
  // Authored smooth normals drive the initial displacement. Once points move,
  // normals are derived from the displaced geometry rather than stale input.
  if (normals) for (std::size_t i = 0; i < vertices.size(); ++i) {
    if ((i & 255) == 0 && canceled && *canceled) return false;
    Vec normal{normals->values[i * 3], normals->values[i * 3 + 1], normals->values[i * 3 + 2]};
    if (skinPose) {
      // Positions use the weighted affine skin transform. Apply the inverse
      // transpose of that same blended linear map, rather than averaging
      // individually normalized joint normals (incorrect for mixed scales).
      Matrix blended{};
      for (const auto &set : influences) for (int component = 0; component < 4; ++component) {
        const auto at = i * 4 + component;
        const double weight = set.weights->values[at];
        if (weight > 0) {
          const auto &joint = skinPose->jointMatrices[std::size_t(set.joints->values[at] + .5)];
          for (int column = 0; column < 3; ++column) for (int row = 0; row < 3; ++row)
            blended[column * 4 + row] += weight * joint[column * 4 + row];
        }
      }
      normal = transformNormal(blended, normal);
    }
    normal = normalized(normal);
    if (dot(normal, normal) > 0) {
      vertices[i].objectNormal = array(normal);
      const auto world = instance.normal(transformNormal(nodeWorld, normal));
      if (dot(world, world) > 0) vertices[i].worldNormal = vertices[i].cameraNormal = array(world);
    }
  }
  const auto color = options.materialColors ? base : std::array<float, 3>{{.75f, .75f, .75f}};
  const std::array<double, 4> baseColor{{color[0], color[1], color[2], 1}};
  bool displaced = false;
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    if ((i & 63) == 0 && canceled && *canceled) return false;
    auto &vertex = vertices[i];
    if (dot(vector(vertex.objectNormal), vector(vertex.objectNormal)) == 0) continue;
    const double distance = evaluator.displace(surfaceSample(vertex, baseColor, *options.surface));
    require(std::isfinite(distance), "Procedural surface displacement is non-finite.");
    if (!distance) continue;
    const Vec moved = add(vector(vertex.objectPosition), scaled(vector(vertex.objectNormal), distance));
    finitePosition(moved);
    vertex.objectPosition = array(moved);
    updatePosition(vertex);
    displaced = true;
  }
  if (displaced && !recomputeNormals()) return false;
  for (const auto &face : triangles) {
    if (canceled && *canceled) return false;
    const auto polygon = clipSurface(clipSurface({vertices[face[0]], vertices[face[1]], vertices[face[2]]},
                                                 options.nearClip, true), options.farClip, false);
    for (std::size_t j = 1; j + 1 < polygon.size(); ++j) {
      require(out.triangles.size() < (MemoryLimit - vertexBytes - indexBytes) / (3 * triangleBytes),
              "Procedural projected geometry exceeds the 256 MiB render budget.");
      RenderTriangle triangle{{{project(vector(polygon[0].cameraPosition)),
                                 project(vector(polygon[j].cameraPosition)),
                                 project(vector(polygon[j + 1].cameraPosition))}},
                               {{j == 1, true, j + 2 == polygon.size()}}, color};
      if (std::abs(edge(triangle.vertices[0], triangle.vertices[1], triangle.vertices[2].x,
                        triangle.vertices[2].y)) < 1e-12) continue;
      auto payload = std::make_shared<SurfaceTriangle>();
      payload->vertices = {{polygon[0], polygon[j], polygon[j + 1]}};
      payload->baseColor = baseColor;
      triangle.surface = std::move(payload);
      if (out.triangles.empty()) {
        const auto &v = triangle.vertices[0];
        out.bounds = {{v.x, v.y, v.x, v.y}};
      }
      for (const auto &v : triangle.vertices) {
        out.bounds[0] = std::min(out.bounds[0], v.x); out.bounds[1] = std::min(out.bounds[1], v.y);
        out.bounds[2] = std::max(out.bounds[2], v.x); out.bounds[3] = std::max(out.bounds[3], v.y);
      }
      out.triangles.push_back(std::move(triangle));
    }
  }
  return true;
}
}  // namespace

bool RenderOptions::operator==(const RenderOptions &b) const {
  return position == b.position && rotation == b.rotation && scale == b.scale &&
         cameraDistance == b.cameraDistance && fieldOfView == b.fieldOfView &&
         orthoHeight == b.orthoHeight && nearClip == b.nearClip &&
         farClip == b.farClip && perspective == b.perspective &&
         headlight == b.headlight && wireframe == b.wireframe &&
         materialColors == b.materialColors && colors == b.colors &&
         useLightingRig == b.useLightingRig && lighting == b.lighting &&
         transforms == b.transforms && animation == b.animation &&
         sourceSeconds == b.sourceSeconds &&
         ((!surface && !b.surface) || (surface && b.surface && *surface == *b.surface));
}

float linearToSrgb(float v) {
  v = std::clamp(v, 0.0f, 1.0f);
  return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
float srgbToLinear(float v) {
  return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
std::array<float, 3> materialColor(const Asset &asset, std::size_t index) {
  if (index >= asset.materials.size()) return {{1, 1, 1}};
  const auto &c = asset.materials[index].baseColor;
  return {{linearToSrgb(c[0]), linearToSrgb(c[1]), linearToSrgb(c[2])}};
}

RenderScene prepareRender(const Asset &asset, const RenderOptions &o,
                          const int *canceled) {
  validateSurface(o.surface);
  for (double v : o.position) require(std::isfinite(v), "Non-finite GLB position.");
  for (double v : o.rotation) require(std::isfinite(v), "Non-finite GLB rotation.");
  for (const auto &additional : o.transforms) {
    for (double v : additional.position)
      require(std::isfinite(v), "Non-finite 3D transform position.");
    for (double v : additional.rotation)
      require(std::isfinite(v), "Non-finite 3D transform rotation.");
    for (double v : additional.scale)
      require(std::isfinite(v) && v > 0.0,
              "3D transform scale must be positive and finite.");
  }
  require(std::isfinite(o.scale) && o.scale > 0 &&
              std::isfinite(o.cameraDistance) && o.cameraDistance > 0 &&
              std::isfinite(o.nearClip) && std::isfinite(o.farClip) &&
              o.nearClip > 0 && o.farClip > o.nearClip,
          "GLB camera requires positive distances and Far Clip greater than Near Clip.");
  require(o.perspective ? (std::isfinite(o.fieldOfView) && o.fieldOfView > 0 && o.fieldOfView < 180)
                        : (std::isfinite(o.orthoHeight) && o.orthoHeight > 0),
          "Invalid GLB projection height or field of view.");
  require(o.animation == NoIndex || std::isfinite(o.sourceSeconds),
          "GLB animation time must be finite.");

  RenderScene out;
  require(o.colors.empty() || o.colors.size() == asset.materials.size() + 1,
          "GLB material color count does not match the asset.");
  for (const auto &color : o.colors)
    for (float v : color) require(std::isfinite(v) && v >= 0 && v <= 1,
                                 "Invalid GLB material color.");
  if (o.useLightingRig) {
    require(std::isfinite(o.lighting.ambient) && o.lighting.ambient >= 0.0 &&
                std::isfinite(o.lighting.master) && o.lighting.master >= 0.0,
            "3D lighting requires finite nonnegative Ambient and Master values.");
    for (const auto &light : o.lighting.lights) {
      double length2 = 0.0;
      for (double v : light.direction) {
        require(std::isfinite(v), "3D light direction must be finite.");
        length2 += v * v;
      }
      require(length2 > 1e-18 && std::isfinite(light.intensity) &&
                  light.intensity >= 0.0,
              "3D directional light requires a nonzero direction and nonnegative intensity.");
      for (float v : light.color)
        require(std::isfinite(v) && v >= 0.0f && v <= 1.0f,
                "3D light color must be finite and in [0,1].");
    }
  }
  out.wireframe = o.wireframe;
  out.surface = o.surface;
  out.perspective = o.perspective;
  out.headlight = o.headlight;
  out.useLightingRig = o.useLightingRig;
  out.lighting = o.lighting;
  if (asset.scenes.empty()) return out;
  if (canceled && *canceled) return {};
  std::unique_ptr<SurfaceEvaluator> surfaceEvaluator;
  if (o.surface) {
    surfaceEvaluator = o.surface->createEvaluator(canceled);
    require(bool(surfaceEvaluator), "Procedural surface worker factory returned no evaluator.");
  }

  PoseResult evaluated;
  const Pose *pose = nullptr;
  if (o.animation != NoIndex) {
    evaluated = evaluatePose(asset, o.animation, o.sourceSeconds);
    if (!evaluated) throw std::runtime_error(evaluated.error);
    pose = &evaluated.pose;
    out.warnings.insert(out.warnings.end(), evaluated.warnings.begin(),
                        evaluated.warnings.end());
  }

  const int scene = asset.defaultScene == NoIndex ? 0 : asset.defaultScene;
  require(scene >= 0 && std::size_t(scene) < asset.scenes.size(), "Invalid GLB scene index.");
  if (asset.defaultScene == NoIndex)
    out.warnings.push_back("No default scene declared; rendering the first scene.");
  const Instance instance(o);
  const double factor = o.perspective ? ProjectionHeight / (2 * std::tan(o.fieldOfView * Pi / 360))
                                      : ProjectionHeight / o.orthoHeight;
  auto project = [&](Vec p) {
    const double k = o.perspective ? factor / -p.z : factor;
    ProjectedVertex v{p.x * k, p.y * k, o.perspective ? 1 / -p.z : p.z};
    require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.depth) &&
                std::abs(v.x) < 1e12 && std::abs(v.y) < 1e12,
            "GLB projection exceeds the supported coordinate range.");
    return v;
  };
  bool skipped = false;
  std::vector<int> pending = asset.scenes[scene].roots;
  std::size_t visited = 0;
  while (!pending.empty()) {
    if (canceled && *canceled) return {};
    const int index = pending.back(); pending.pop_back();
    require(index >= 0 && std::size_t(index) < asset.nodes.size() &&
                ++visited <= asset.nodes.size(), "Invalid GLB scene hierarchy.");
    const Node &node = asset.nodes[index];
    const Matrix &nodeWorld = pose ? pose->nodes[index].world : node.world;
    pending.insert(pending.end(), node.children.begin(), node.children.end());
    if (node.mesh == NoIndex) continue;
    require(node.mesh >= 0 && std::size_t(node.mesh) < asset.meshes.size(), "Invalid GLB mesh index.");

    const SkinPose *skinPose = nullptr;
    if (pose && node.hasSkin) {
      const auto found = std::find_if(pose->skins.begin(), pose->skins.end(),
          [&](const SkinPose &candidate) { return candidate.node == index; });
      require(found != pose->skins.end(),
              "GLB animated skinned node has no evaluated skin binding.");
      skinPose = &*found;
    }

    for (const Primitive &p : asset.meshes[node.mesh].primitives) {
      require(p.material == NoIndex || (p.material >= 0 &&
                  std::size_t(p.material) < asset.materials.size()),
              "Invalid GLB material index.");
      const std::size_t material = p.material == NoIndex ? asset.materials.size() : p.material;
      const auto base = o.colors.empty() ? materialColor(asset, material) : o.colors[material];
      if (p.mode < 4 || p.mode > 6) { skipped = true; continue; }
      if (surfaceEvaluator) {
        if (!appendSurfacePrimitive(out, p, nodeWorld, skinPose, o, instance, project,
                                     base, *surfaceEvaluator, canceled)) return {};
        continue;
      }
      const auto pos = std::find_if(p.attributes.begin(), p.attributes.end(),
          [](const Attribute &a) { return a.semantic == "POSITION"; });
      require(pos != p.attributes.end() && pos->components == 3,
              "GLB triangle geometry has no positions.");

      const auto influences = skinPose ? influenceSets(p) : std::vector<InfluenceSet>();
      if (skinPose)
        require(!influences.empty(),
                "GLB animated skinned primitive has no joint/weight influences.");

      auto vertex = [&](std::size_t i) {
        if (!p.indices.empty()) i = p.indices.at(i);
        require(i < pos->values.size() / 3, "GLB triangle index is out of range.");
        const Vec basePosition{pos->values[i * 3], pos->values[i * 3 + 1],
                               pos->values[i * 3 + 2]};
        Vec local = basePosition;
        if (skinPose) {
          Vec weighted{0, 0, 0};
          double total = 0.0;
          for (const auto &set : influences) {
            for (int component = 0; component < 4; ++component) {
              const std::size_t at = i * 4 + component;
              const double weight = set.weights->values[at];
              if (!(weight > 0.0)) continue;
              const double jointValue = set.joints->values[at];
              const auto joint = std::size_t(std::llround(jointValue));
              require(std::isfinite(jointValue) && jointValue >= 0.0 &&
                          std::abs(jointValue - double(joint)) < 1e-6 &&
                          joint < skinPose->jointMatrices.size(),
                      "GLB skin influence references an invalid joint.");
              const Vec moved = transform(skinPose->jointMatrices[joint], basePosition);
              weighted.x += weight * moved.x;
              weighted.y += weight * moved.y;
              weighted.z += weight * moved.z;
              total += weight;
            }
          }
          require(total > 1e-12 && std::isfinite(total),
                  "GLB skinned vertex has no positive joint weight.");
          local = {weighted.x / total, weighted.y / total, weighted.z / total};
        }
        return instance(transform(nodeWorld, local));
      };
      const std::size_t count = p.indices.empty() ? p.vertexCount : p.indices.size();
      for (std::size_t i = 2; i < count; i += p.mode == 4 ? 3 : 1) {
        if ((i & 1023) == 0 && canceled && *canceled) return {};
        const Vec a = vertex(p.mode == 6 ? 0 : i - 2), b = vertex(i - 1), c = vertex(i);
        Vec normal = cross(sub(b, a), sub(c, a));
        const double length = std::sqrt(dot(normal, normal));
        if (length < 1e-15) continue;
        const Vec view = o.perspective ? Vec{-(a.x + b.x + c.x), -(a.y + b.y + c.y), -(a.z + b.z + c.z)}
                                       : Vec{0, 0, 1};
        const double viewLength = std::sqrt(dot(view, view));
        const double denominator = length * viewLength;
        const double light = denominator > 0 && std::isfinite(denominator)
            ? std::abs(dot(normal, view)) / denominator : 0;
        const float gray = o.headlight ? float(0.2 + 0.6 * std::clamp(light, 0.0, 1.0)) : 0.75f;
        std::array<float, 3> color{{gray, gray, gray}};
        if (o.materialColors) {
          color = base;
          if (o.headlight)
            for (auto &v : color) v = linearToSrgb(srgbToLinear(v) * gray);
        }

        if (o.useLightingRig) {
          // The renderer is intentionally two-sided. Orient the face normal
          // toward the camera before evaluating camera-relative diffuse lights.
          if (viewLength > 0.0 && dot(normal, view) < 0.0)
            normal = {-normal.x, -normal.y, -normal.z};
          normal = {normal.x / length, normal.y / length, normal.z / length};
          const std::array<float, 3> surface =
              o.materialColors ? base : std::array<float, 3>{{0.75f, 0.75f, 0.75f}};
          std::array<double, 3> illumination{{o.lighting.ambient,
                                              o.lighting.ambient,
                                              o.lighting.ambient}};
          for (const auto &source : o.lighting.lights) {
            const double l2 = source.direction[0] * source.direction[0] +
                              source.direction[1] * source.direction[1] +
                              source.direction[2] * source.direction[2];
            const double invLength = 1.0 / std::sqrt(l2);
            const Vec direction{source.direction[0] * invLength,
                                source.direction[1] * invLength,
                                source.direction[2] * invLength};
            const double diffuse = std::max(0.0, dot(normal, direction)) *
                                   source.intensity;
            for (int channel = 0; channel < 3; ++channel)
              illumination[channel] += diffuse * srgbToLinear(source.color[channel]);
          }
          for (int channel = 0; channel < 3; ++channel) {
            const double linear = srgbToLinear(surface[channel]) *
                                  illumination[channel] * o.lighting.master;
            color[channel] = linearToSrgb(float(linear));
          }
        }

        const auto polygon = clip(clip({a, b, c}, o.nearClip, true), o.farClip, false);
        for (std::size_t j = 1; j + 1 < polygon.size(); ++j) {
          // Include both old and new allocations during vector growth.
          require(out.triangles.size() < MemoryLimit / (3 * sizeof(RenderTriangle)),
                  "GLB projected geometry exceeds the 256 MiB render budget.");
          RenderTriangle triangle{{{project(polygon[0]), project(polygon[j]), project(polygon[j + 1])}},
                                  {{j == 1, true, j + 2 == polygon.size()}}, color};
          if (std::abs(edge(triangle.vertices[0], triangle.vertices[1],
                            triangle.vertices[2].x, triangle.vertices[2].y)) < 1e-12) continue;
          if (out.triangles.empty()) {
            const auto &v = triangle.vertices[0];
            out.bounds = {{v.x, v.y, v.x, v.y}};
          }
          for (const auto &v : triangle.vertices) {
            out.bounds[0] = std::min(out.bounds[0], v.x); out.bounds[1] = std::min(out.bounds[1], v.y);
            out.bounds[2] = std::max(out.bounds[2], v.x); out.bounds[3] = std::max(out.bounds[3], v.y);
          }
          out.triangles.push_back(triangle);
        }
      }
    }
  }
  if (skipped) out.warnings.push_back("Point/line primitives are not rendered; triangle surfaces only.");
  return out;
}

namespace {
SurfaceVertex interpolateSample(const SurfaceTriangle &triangle, double a, double b, double c) {
  SurfaceVertex result;
  const auto combine = [=](const auto &first, const auto &second, const auto &third, auto &out) {
    for (std::size_t component = 0; component < out.size(); ++component)
      out[component] = a * first[component] + b * second[component] + c * third[component];
  };
  const auto &x = triangle.vertices[0], &y = triangle.vertices[1], &z = triangle.vertices[2];
  combine(x.objectPosition, y.objectPosition, z.objectPosition, result.objectPosition);
  combine(x.worldPosition, y.worldPosition, z.worldPosition, result.worldPosition);
  combine(x.cameraPosition, y.cameraPosition, z.cameraPosition, result.cameraPosition);
  combine(x.objectNormal, y.objectNormal, z.objectNormal, result.objectNormal);
  combine(x.worldNormal, y.worldNormal, z.worldNormal, result.worldNormal);
  combine(x.cameraNormal, y.cameraNormal, z.cameraNormal, result.cameraNormal);
  combine(x.uv, y.uv, z.uv, result.uv);
  return result;
}

bool coverageEdge(double value, ProjectedVertex a, ProjectedVertex b, double sign,
                  double epsilon) {
  value *= sign;
  if (value < -epsilon) return false;
  if (value > epsilon) return true;
  if (sign < 0) std::swap(a, b);
  // Exactly one triangle owns an edge shared with its reversed neighbor. This
  // prevents a translucent surface from blending with itself on mesh seams.
  const double dy = b.y - a.y, dx = b.x - a.x;
  return dy > 0 || (dy == 0 && dx < 0);
}

std::array<double, 3> illuminate(const RenderScene &scene, const SurfaceSample &sample,
                                std::array<double, 3> color) {
  if (!scene.useLightingRig && !scene.headlight) return color;
  Vec normal = normalized(vector(sample.cameraNormal));
  const Vec view = scene.perspective ? scaled(vector(sample.cameraPosition), -1) : Vec{0, 0, 1};
  const Vec direction = normalized(view);
  if (dot(normal, direction) < 0) normal = scaled(normal, -1);
  std::array<double, 3> gain{};
  if (scene.useLightingRig) {
    gain.fill(scene.lighting.ambient);
    for (const auto &light : scene.lighting.lights) {
      const auto toLight = normalized(vector(light.direction));
      const double amount = std::max(0.0, dot(normal, toLight)) * light.intensity;
      for (int channel = 0; channel < 3; ++channel)
        gain[channel] += amount * srgbToLinear(light.color[channel]);
    }
    for (auto &value : gain) value *= scene.lighting.master;
  } else gain.fill(.2 + .6 * std::clamp(std::abs(dot(normal, direction)), 0.0, 1.0));
  for (int channel = 0; channel < 3; ++channel) {
    const double value = srgbToLinear(float(color[channel])) * gain[channel];
    require(std::isfinite(value), "Procedural surface illumination is non-finite.");
    color[channel] = linearToSrgb(float(value));
  }
  return color;
}

std::vector<ColorPixel> renderSurfaceTile(const RenderScene &scene, const RenderTile &tile,
                                         const int *canceled) {
  validateSurface(scene.surface);
  const std::size_t count = std::size_t(tile.width) * std::size_t(tile.height);
  struct Fragment {
    std::size_t sample, triangle;
    double depth;
    std::array<double, 3> opacity;
    ColorPixel color;
    bool painted;
  };
  require(count <= MemoryLimit / (sizeof(ColorPixel) + 4 * sizeof(Fragment)),
          "Procedural surface tile exceeds the 256 MiB render budget; reduce the tile size.");
  std::vector<ColorPixel> output(count);
  if (!count || scene.triangles.empty()) return output;
  if (canceled && *canceled) return {};
  auto evaluator = scene.surface->createEvaluator(canceled);
  require(bool(evaluator), "Procedural surface worker factory returned no evaluator.");
  const auto available = MemoryLimit - count * sizeof(ColorPixel);
  // Account for old and new vector storage during explicit capacity growth.
  const auto maxFragments = available / (2 * sizeof(Fragment));
  std::vector<Fragment> fragments;
  fragments.reserve(std::min(maxFragments, count * 4));
  std::size_t attemptedSamples = 0;
  const auto &m = tile.affine;
  for (std::size_t ordinal = 0; ordinal < scene.triangles.size(); ++ordinal) {
    if (canceled && *canceled) return {};
    const auto &triangle = scene.triangles[ordinal];
    require(bool(triangle.surface), "Procedural surface triangle lost its shading context.");
    auto vertices = triangle.vertices;
    for (auto &p : vertices) {
      const double x = m[0] * p.x + m[1] * p.y + m[2] - tile.x;
      p.y = m[3] * p.x + m[4] * p.y + m[5] - tile.y;
      p.x = x;
      require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.depth),
              "Procedural surface image affine overflows.");
    }
    const double area = edge(vertices[0], vertices[1], vertices[2].x, vertices[2].y);
    require(std::isfinite(area), "Procedural surface projected area overflows.");
    if (std::abs(area) < 1e-12) continue;
    const double x0 = std::min({vertices[0].x, vertices[1].x, vertices[2].x});
    const double x1 = std::max({vertices[0].x, vertices[1].x, vertices[2].x});
    const double y0 = std::min({vertices[0].y, vertices[1].y, vertices[2].y});
    const double y1 = std::max({vertices[0].y, vertices[1].y, vertices[2].y});
    if (x1 < 0 || y1 < 0 || x0 >= tile.width || y0 >= tile.height) continue;
    const int left = int(std::max(0.0, std::floor(x0)));
    const int bottom = int(std::max(0.0, std::floor(y0)));
    const int right = int(std::min(double(tile.width - 1), std::floor(x1)));
    const int top = int(std::min(double(tile.height - 1), std::floor(y1)));
    const double lengths[] = {std::hypot(vertices[1].x - vertices[0].x, vertices[1].y - vertices[0].y),
                              std::hypot(vertices[2].x - vertices[1].x, vertices[2].y - vertices[1].y),
                              std::hypot(vertices[0].x - vertices[2].x, vertices[0].y - vertices[2].y)};
    const double sign = area > 0 ? 1 : -1;
    const double epsilon = 1e-12 * std::max(1.0, std::abs(area));
    for (int y = bottom; y <= top; ++y) {
      if (canceled && *canceled) return {};
      for (int x = left; x <= right; ++x) for (int s = 0; s < 4; ++s) {
        // Count attempted coverage work too: very thin triangles can span a
        // large bounding rectangle without covering any quarter-pixel sample.
        require(++attemptedSamples <= 64ull * 1024 * 1024,
                "Procedural surface coverage exceeds the sample budget; reduce the tile size.");
        if ((attemptedSamples & 63) == 0 && canceled && *canceled) return {};
        const double px = x + .25 + .5 * (s & 1), py = y + .25 + .5 * (s >> 1);
        const double e0 = edge(vertices[0], vertices[1], px, py);
        const double e1 = edge(vertices[1], vertices[2], px, py);
        const double e2 = edge(vertices[2], vertices[0], px, py);
        if (!coverageEdge(e0, vertices[0], vertices[1], sign, epsilon) ||
            !coverageEdge(e1, vertices[1], vertices[2], sign, epsilon) ||
            !coverageEdge(e2, vertices[2], vertices[0], sign, epsilon)) continue;
        double a = e1 / area, b = e2 / area, c = e0 / area;
        const double depth = a * vertices[0].depth + b * vertices[1].depth + c * vertices[2].depth;
        if (scene.perspective) {
          require(std::isfinite(depth) && depth > 0, "Invalid procedural surface perspective depth.");
          a *= triangle.vertices[0].depth / depth;
          b *= triangle.vertices[1].depth / depth;
          c *= triangle.vertices[2].depth / depth;
        }
        const auto vertex = interpolateSample(*triangle.surface, a, b, c);
        auto base = triangle.surface->baseColor;
        if (triangle.textured) {
          base = {{0, 0, 0, 0}};
          const double u = vertex.uv[0], v = vertex.uv[1];
          if (scene.texture && scene.textureWidth > 0 && scene.textureHeight > 0 &&
              u >= 0 && u <= 1 && v >= 0 && v <= 1) {
            const int tx = std::min(scene.textureWidth - 1, int(u * scene.textureWidth));
            const int ty = std::min(scene.textureHeight - 1, int(v * scene.textureHeight));
            require(scene.texture->size() == std::size_t(scene.textureWidth) * scene.textureHeight,
                    "Procedural surface texture shape is invalid.");
            const auto &pixel = (*scene.texture)[std::size_t(ty) * scene.textureWidth + tx];
            for (double value : {double(pixel.r), double(pixel.g), double(pixel.b), double(pixel.alpha)})
              require(std::isfinite(value), "Non-finite procedural surface input pixel.");
            base[3] = std::clamp(double(pixel.alpha), 0.0, 1.0);
            if (base[3] > 0) base = {{pixel.r / base[3], pixel.g / base[3], pixel.b / base[3], base[3]}};
          }
        }
        const auto sample = surfaceSample(vertex, base, *scene.surface);
        auto shaded = evaluator->shade(sample);
        require(std::isfinite(shaded.opacity), "Procedural surface opacity is non-finite.");
        const double scalarOpacity = std::clamp(shaded.opacity, 0.0, 1.0);
        std::array<double, 3> opacity;
        for (int channel = 0; channel < 3; ++channel) {
          require(std::isfinite(shaded.coloredOpacity[channel]),
                  "Procedural surface RGB opacity is non-finite.");
          opacity[channel] = scalarOpacity *
              std::clamp(shaded.coloredOpacity[channel], 0.0, 1.0);
        }
        for (double value : shaded.color)
          require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
                  "Procedural surface color is outside the finite output range.");
        if (!opacity[0] && !opacity[1] && !opacity[2]) continue;
        shaded.color = illuminate(scene, sample, shaded.color);
        const bool line = !scene.wireframe ||
            (triangle.edges[0] && std::abs(e0) <= .65 * lengths[0]) ||
            (triangle.edges[1] && std::abs(e1) <= .65 * lengths[1]) ||
            (triangle.edges[2] && std::abs(e2) <= .65 * lengths[2]);
        ColorPixel color;
        if (line) color = {float(shaded.color[0] * opacity[0]), float(shaded.color[1] * opacity[1]),
                           float(shaded.color[2] * opacity[2]),
                           float(std::max({opacity[0], opacity[1], opacity[2]}))};
        require(fragments.size() < maxFragments,
                "Procedural transparent overlap exceeds the 256 MiB budget; reduce the tile size.");
        if (fragments.size() == fragments.capacity())
          fragments.reserve(std::min(maxFragments, std::max(std::size_t(64), fragments.capacity() * 2)));
        fragments.push_back({(std::size_t(y) * tile.width + x) * 4 + s,
                             ordinal, depth, opacity, color, line});
      }
    }
  }
  std::sort(fragments.begin(), fragments.end(), [](const Fragment &a, const Fragment &b) {
    if (a.sample != b.sample) return a.sample < b.sample;
    if (a.depth != b.depth) return a.depth > b.depth;
    return a.triangle < b.triangle;
  });
  for (std::size_t first = 0; first < fragments.size();) {
    if (canceled && *canceled) return {};
    const std::size_t sample = fragments[first].sample;
    double r = 0, g = 0, b = 0, legacyAlpha = 0;
    std::array<double, 3> transmittance{{1, 1, 1}}, paintedCoverage{};
    bool colored = false;
    std::size_t next = first;
    for (; next < fragments.size() && fragments[next].sample == sample; ++next) {
      const auto &fragment = fragments[next];
      r += transmittance[0] * fragment.color.r;
      g += transmittance[1] * fragment.color.g;
      b += transmittance[2] * fragment.color.b;
      // Keep the established scalar path's rounding when all channels agree.
      legacyAlpha += transmittance[0] * fragment.color.alpha;
      colored = colored || fragment.opacity[0] != fragment.opacity[1] ||
                            fragment.opacity[0] != fragment.opacity[2];
      for (int channel = 0; channel < 3; ++channel) {
        if (fragment.painted)
          paintedCoverage[channel] += transmittance[channel] * fragment.opacity[channel];
        transmittance[channel] *= 1.0 - fragment.opacity[channel];
      }
    }
    // Native RGBA has one alpha. Preserve colored transmission inside the 3D
    // stack, then flatten it to maximum coverage. Hidden wireframe fill still
    // occludes farther lines without becoming visible coverage itself.
    const double alpha = !colored ? legacyAlpha : scene.wireframe
        ? std::max({paintedCoverage[0], paintedCoverage[1], paintedCoverage[2]})
        : 1.0 - std::min({transmittance[0], transmittance[1], transmittance[2]});
    for (double value : {r, g, b})
      require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
              "Procedural surface composition is outside the finite output range.");
    auto &pixel = output[sample / 4];
    pixel.r += float(r * .25); pixel.g += float(g * .25);
    pixel.b += float(b * .25); pixel.alpha += float(alpha * .25);
    first = next;
  }
  return output;
}
}  // namespace

std::vector<ColorPixel> renderTile(const RenderScene &scene, const RenderTile &tile,
                                   const int *canceled) {
  require(tile.width >= 0 && tile.height >= 0, "Invalid GLB tile dimensions.");
  for (double value : tile.affine) require(std::isfinite(value), "Invalid GLB image affine.");
  require(std::isfinite(tile.x) && std::isfinite(tile.y), "Invalid GLB tile origin.");
  if (scene.surface) return renderSurfaceTile(scene, tile, canceled);
  const std::size_t count = std::size_t(tile.width) * std::size_t(tile.height);
  struct Sample { double depth = -std::numeric_limits<double>::infinity(); ColorPixel color; };
  require(count <= MemoryLimit / (sizeof(ColorPixel) + 4 * sizeof(Sample)),
          "GLB tile exceeds the 256 MiB render budget; reduce the render tile size.");
  for (double value : tile.affine) require(std::isfinite(value), "Invalid GLB image affine.");
  require(std::isfinite(tile.x) && std::isfinite(tile.y), "Invalid GLB tile origin.");
  std::vector<ColorPixel> output(count);
  if (!count || scene.triangles.empty()) return output;
  std::vector<Sample> samples(count * 4);
  const auto &m = tile.affine;
  for (const auto &triangle : scene.triangles) {
    if (canceled && *canceled) return {};
    auto v = triangle.vertices;
    for (auto &p : v) {
      const double x = m[0] * p.x + m[1] * p.y + m[2] - tile.x;
      p.y = m[3] * p.x + m[4] * p.y + m[5] - tile.y; p.x = x;
      require(std::isfinite(p.x) && std::isfinite(p.y), "GLB image affine overflows.");
    }
    const double area = edge(v[0], v[1], v[2].x, v[2].y);
    if (std::abs(area) < 1e-12) continue;
    double x0 = std::min({v[0].x, v[1].x, v[2].x}), x1 = std::max({v[0].x, v[1].x, v[2].x});
    double y0 = std::min({v[0].y, v[1].y, v[2].y}), y1 = std::max({v[0].y, v[1].y, v[2].y});
    if (x1 < 0 || y1 < 0 || x0 >= tile.width || y0 >= tile.height) continue;
    const int left = int(std::max(0.0, std::floor(x0))), bottom = int(std::max(0.0, std::floor(y0)));
    const int right = int(std::min(double(tile.width - 1), std::floor(x1)));
    const int top = int(std::min(double(tile.height - 1), std::floor(y1)));
    const double lengths[] = {std::hypot(v[1].x - v[0].x, v[1].y - v[0].y),
                              std::hypot(v[2].x - v[1].x, v[2].y - v[1].y),
                              std::hypot(v[0].x - v[2].x, v[0].y - v[2].y)};
    for (int y = bottom; y <= top; ++y) {
      if (canceled && *canceled) return {};
      for (int x = left; x <= right; ++x) for (int s = 0; s < 4; ++s) {
        const double px = x + 0.25 + 0.5 * (s & 1), py = y + 0.25 + 0.5 * (s >> 1);
        const double e0 = edge(v[0], v[1], px, py), e1 = edge(v[1], v[2], px, py), e2 = edge(v[2], v[0], px, py);
        const double a = e1 / area, b = e2 / area, c = e0 / area;
        if (a < -1e-12 || b < -1e-12 || c < -1e-12) continue;
        const double depth = a * v[0].depth + b * v[1].depth + c * v[2].depth;
        Sample &sample = samples[(std::size_t(y) * tile.width + x) * 4 + s];
        const bool line = !scene.wireframe ||
            (triangle.edges[0] && std::abs(e0) <= 0.65 * lengths[0]) ||
            (triangle.edges[1] && std::abs(e1) <= 0.65 * lengths[1]) ||
            (triangle.edges[2] && std::abs(e2) <= 0.65 * lengths[2]);
        const double epsilon = 1e-10 * std::max(1.0, std::abs(depth));
        if (depth > sample.depth + epsilon) {
          ColorPixel color{triangle.color[0], triangle.color[1], triangle.color[2], 1};
          if (triangle.textured) {
            color = {};
            if (scene.texture && scene.textureWidth > 0 && scene.textureHeight > 0) {
              // The plane uses an orthographic camera; UVs interpolate linearly.
              {
                const double u = a * triangle.uv[0][0] +
                                 b * triangle.uv[1][0] +
                                 c * triangle.uv[2][0];
                const double v = a * triangle.uv[0][1] +
                                 b * triangle.uv[1][1] +
                                 c * triangle.uv[2][1];
                if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                  const int tx = std::min(scene.textureWidth - 1, int(u * scene.textureWidth));
                  const int ty = std::min(scene.textureHeight - 1, int(v * scene.textureHeight));
                  color = (*scene.texture)[std::size_t(ty) * scene.textureWidth + tx];
                }
              }
            }
          }
          // Transparent texels do not occlude other geometry.
          if (color.alpha > 0) sample = {depth, line ? color : ColorPixel{}};
        } else if (scene.wireframe && line && std::abs(depth - sample.depth) <= epsilon) {
          sample.color = {triangle.color[0], triangle.color[1], triangle.color[2], 1};
        }
      }
    }
  }
  for (std::size_t i = 0; i < count; ++i) for (int s = 0; s < 4; ++s) {
    const auto &c = samples[i * 4 + s].color;
    output[i].r += c.r * 0.25f;
    output[i].g += c.g * 0.25f;
    output[i].b += c.b * 0.25f;
    output[i].alpha += c.alpha * 0.25f;
  }
  return output;
}

RenderScene prepareImagePlane(
    int width, int height,
    std::shared_ptr<const std::vector<ColorPixel>> pixels,
    const std::vector<ModelTransform> &transforms,
    std::shared_ptr<const SurfaceRequest> surface, const int *canceled) {
  require(width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
              (!pixels || pixels->size() == std::size_t(width) * height),
          "Invalid image plane texture dimensions.");
  for (const auto &t : transforms) {
    for (double v : t.position) require(std::isfinite(v), "Invalid image plane position.");
    for (double v : t.rotation) require(std::isfinite(v), "Invalid image plane rotation.");
    for (double v : t.scale)
      require(std::isfinite(v) && v > 0, "Invalid image plane scale.");
  }
  validateSurface(surface);
  if (canceled && *canceled) return {};
  if (surface) {
    // Reuse the same preprojection displacement/clipping path. The geometry
    // query supplies no raster and never invokes the per-sample shade callback.
    Asset asset;
    Primitive primitive;
    primitive.vertexCount = 4;
    primitive.triangleCount = 2;
    const float x = float(width / 200.0), y = float(height / 200.0);
    primitive.attributes.push_back({"POSITION", 3, {-x, -y, 0, x, -y, 0, x, y, 0, -x, y, 0}});
    primitive.attributes.push_back({"TEXCOORD_0", 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    primitive.indices = {0, 1, 2, 0, 2, 3};
    asset.meshes.push_back({"", {std::move(primitive)}, {}});
    Node node;
    node.mesh = 0;
    node.local[0] = node.local[5] = node.local[10] = node.local[15] = 1;
    node.world = node.local;
    asset.nodes.push_back(std::move(node));
    asset.scenes.push_back({"", {0}, {}});
    asset.defaultScene = 0;
    RenderOptions options;
    options.materialColors = true;
    options.transforms = transforms;
    options.surface = std::move(surface);
    auto result = prepareRender(asset, options, canceled);
    result.textureWidth = width;
    result.textureHeight = height;
    result.texture = std::move(pixels);
    for (auto &triangle : result.triangles) triangle.textured = true;
    return result;
  }
  RenderScene scene;
  scene.textureWidth = width;
  scene.textureHeight = height;
  scene.texture = std::move(pixels);
  std::array<Vec, 4> corners{{{-width / 200.0, -height / 200.0, 0},
                              {width / 200.0, -height / 200.0, 0},
                              {width / 200.0, height / 200.0, 0},
                              {-width / 200.0, height / 200.0, 0}}};
  std::array<ProjectedVertex, 4> projected;
  for (int i = 0; i < 4; ++i) {
    Vec p = corners[i];
    for (const auto &t : transforms) p = transform(p, t);
    // Keep geometry behind or crossing the camera out of the rasterizer.
    if (p.z >= 9.9) return scene;
    projected[i] = {p.x * 100.0, p.y * 100.0, p.z - 10.0};
  }
  const std::array<std::array<double, 2>, 4> uv{{{{0, 0}}, {{1, 0}},
                                                  {{1, 1}}, {{0, 1}}}};
  for (const auto indices : {std::array<int, 3>{{0, 1, 2}},
                              std::array<int, 3>{{0, 2, 3}}}) {
    RenderTriangle triangle;
    triangle.vertices = {{projected[indices[0]], projected[indices[1]],
                          projected[indices[2]]}};
    triangle.edges = {{true, true, true}};
    triangle.color = {{1, 1, 1}};
    triangle.uv = {{uv[indices[0]], uv[indices[1]], uv[indices[2]]}};
    triangle.textured = true;
    scene.triangles.push_back(triangle);
  }
  scene.bounds = {{projected[0].x, projected[0].y,
                   projected[0].x, projected[0].y}};
  for (const auto &v : projected) {
    scene.bounds[0] = std::min(scene.bounds[0], v.x);
    scene.bounds[1] = std::min(scene.bounds[1], v.y);
    scene.bounds[2] = std::max(scene.bounds[2], v.x);
    scene.bounds[3] = std::max(scene.bounds[3], v.y);
  }
  return scene;
}

RenderScene projectImagePlane(
    int width, int height,
    const std::vector<ModelTransform> &transforms,
    std::shared_ptr<const SurfaceRequest> surface, const int *canceled) {
  return prepareImagePlane(width, height, {}, transforms, std::move(surface), canceled);
}
}  // namespace otglb
