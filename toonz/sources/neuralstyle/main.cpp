// Native CPU inference worker. Protocol details are documented with the tests.
// The parent verifies the packaged runtime/model manifest before starting this
// process. No Python, network access, model downloads or executable checkpoints.
#include "litert/c/litert_compiled_model.h"
#include "litert/c/litert_environment.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_options.h"
#include "litert/c/litert_tensor_buffer.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dlfcn.h>
#include <sys/resource.h>
#endif

namespace {
constexpr uint32_t Magic = 0x31535a47;  // "GZS1", little-endian protocol v1.
constexpr size_t StyleCount = 256 * 256 * 3;
constexpr size_t ContentCount = 384 * 384 * 3;
constexpr size_t EmbeddingCount = 100;

struct MemoryReport {
  ~MemoryReport() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
      std::cerr << "peak_memory_bytes=" << counters.PeakWorkingSetSize << '\n';
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
      std::cerr << "peak_memory_bytes=" << usage.ru_maxrss << '\n';
#else
      std::cerr << "peak_memory_bytes=" << uint64_t(usage.ru_maxrss) * 1024 << '\n';
#endif
    }
#endif
  }
};

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void check(LiteRtStatus status) {
  if (status != kLiteRtStatusOk)
    throw std::runtime_error("LiteRT status " + std::to_string(int(status)));
}

class Library {
#ifdef _WIN32
  HMODULE handle = nullptr;
#else
  void *handle = nullptr;
#endif
public:
  explicit Library(const std::filesystem::path &path) {
    require(path.is_absolute(), "Runtime path must be absolute");
#ifdef _WIN32
    handle = LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    require(handle != nullptr, "Cannot load packaged native LiteRT library");
  }
  ~Library() {
#ifdef _WIN32
    if (handle) FreeLibrary(handle);
#else
    if (handle) dlclose(handle);
#endif
  }
  Library(const Library &) = delete;
  Library &operator=(const Library &) = delete;
  template<class T> T resolve(const char *name) {
#ifdef _WIN32
    auto symbol = GetProcAddress(handle, name);
#else
    auto symbol = dlsym(handle, name);
#endif
    if (!symbol) throw std::runtime_error(std::string("Missing LiteRT symbol: ") + name);
    return reinterpret_cast<T>(symbol);
  }
};

#define LITERT_FUNCTIONS(F) \
  F(LiteRtCreateEnvironment) F(LiteRtDestroyEnvironment) \
  F(LiteRtCreateModelFromBuffer) F(LiteRtDestroyModel) \
  F(LiteRtCreateOptions) F(LiteRtDestroyOptions) F(LiteRtSetOptionsHardwareAccelerators) \
  F(LiteRtCreateCompiledModel) F(LiteRtDestroyCompiledModel) \
  F(LiteRtGetModelSignature) F(LiteRtGetNumSignatureInputs) F(LiteRtGetNumSignatureOutputs) \
  F(LiteRtGetSignatureInputTensorByIndex) F(LiteRtGetSignatureOutputTensorByIndex) \
  F(LiteRtGetRankedTensorType) F(LiteRtGetCompiledModelInputBufferRequirements) \
  F(LiteRtGetCompiledModelOutputBufferRequirements) \
  F(LiteRtCreateManagedTensorBufferFromRequirements) F(LiteRtDestroyTensorBuffer) \
  F(LiteRtLockTensorBuffer) F(LiteRtUnlockTensorBuffer) F(LiteRtRunCompiledModel)

struct Api {
  Library library;
#define DECLARE_FUNCTION(name) decltype(&::name) name = nullptr;
  LITERT_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION
  explicit Api(const std::filesystem::path &path) : library(path) {
#define LOAD_FUNCTION(name) name = library.resolve<decltype(name)>(#name);
    LITERT_FUNCTIONS(LOAD_FUNCTION)
#undef LOAD_FUNCTION
  }
};

struct Environment {
  Api &api;
  LiteRtEnvironment value = nullptr;
  explicit Environment(Api &api) : api(api) { check(api.LiteRtCreateEnvironment(0, nullptr, &value)); }
  ~Environment() { if (value) api.LiteRtDestroyEnvironment(value); }
};

struct Tensor {
  LiteRtTensorBuffer buffer = nullptr;
  size_t count = 0;
  std::array<int32_t, 4> shape{};
};

class Model {
  Api &api;
  Environment &environment;
  std::vector<unsigned char> bytes;
  LiteRtModel model = nullptr;
  LiteRtOptions options = nullptr;
  LiteRtCompiledModel compiled = nullptr;
  std::vector<Tensor> inputs, outputs;

  void addTensor(LiteRtSignature signature, LiteRtParamIndex index, bool input) {
    auto &tensors = input ? inputs : outputs;
    LiteRtTensor tensor = nullptr;
    check(input ? api.LiteRtGetSignatureInputTensorByIndex(signature, index, &tensor)
                : api.LiteRtGetSignatureOutputTensorByIndex(signature, index, &tensor));
    LiteRtRankedTensorType type{};
    check(api.LiteRtGetRankedTensorType(tensor, &type));
    require(type.element_type == kLiteRtElementTypeFloat32 && type.layout.rank == 4 &&
            !type.layout.has_strides, "Unsupported model tensor contract");
    Tensor item;
    item.count = 1;
    for (int i = 0; i < 4; ++i) {
      const int32_t dimension = type.layout.dimensions[i];
      require(dimension > 0 && dimension <= 512, "Model dimension exceeds worker bound");
      item.shape[i] = dimension;
      item.count *= size_t(dimension);
      require(item.count <= ContentCount, "Model tensor exceeds worker bound");
    }
    LiteRtTensorBufferRequirements requirements = nullptr;
    check(input ? api.LiteRtGetCompiledModelInputBufferRequirements(compiled, 0, index, &requirements)
                : api.LiteRtGetCompiledModelOutputBufferRequirements(compiled, 0, index, &requirements));
    tensors.push_back(item);
    check(api.LiteRtCreateManagedTensorBufferFromRequirements(
        environment.value, &type, requirements, &tensors.back().buffer));
  }

public:
  Model(Api &api, Environment &environment) : api(api), environment(environment) {}
  Model(const Model &) = delete;
  Model &operator=(const Model &) = delete;
  ~Model() {
    for (const auto &tensor : inputs) if (tensor.buffer) api.LiteRtDestroyTensorBuffer(tensor.buffer);
    for (const auto &tensor : outputs) if (tensor.buffer) api.LiteRtDestroyTensorBuffer(tensor.buffer);
    if (compiled) api.LiteRtDestroyCompiledModel(compiled);
    if (options) api.LiteRtDestroyOptions(options);
    if (model) api.LiteRtDestroyModel(model);
  }
  void initialize(const std::filesystem::path &path, bool prediction) {
    const auto length = std::filesystem::file_size(path);
    require(length >= 8 && length <= 4 * 1024 * 1024, "Invalid model size");
    bytes.resize(size_t(length));
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()))),
            "Cannot read complete model");
    require(std::memcmp(bytes.data() + 4, "TFL3", 4) == 0, "Not a TFLite model");
    check(api.LiteRtCreateModelFromBuffer(environment.value, bytes.data(), bytes.size(), &model));
    check(api.LiteRtCreateOptions(&options));
    check(api.LiteRtSetOptionsHardwareAccelerators(options, kLiteRtHwAcceleratorCpu));
    check(api.LiteRtCreateCompiledModel(environment.value, model, options, &compiled));
    LiteRtSignature signature = nullptr;
    check(api.LiteRtGetModelSignature(model, 0, &signature));
    LiteRtParamIndex inputCount = 0, outputCount = 0;
    check(api.LiteRtGetNumSignatureInputs(signature, &inputCount));
    check(api.LiteRtGetNumSignatureOutputs(signature, &outputCount));
    require(inputCount == (prediction ? 1 : 2) && outputCount == 1, "Unexpected model input/output count");
    for (LiteRtParamIndex i = 0; i < inputCount; ++i) addTensor(signature, i, true);
    addTensor(signature, 0, false);
    const std::array<int32_t, 4> styleShape{{1, 256, 256, 3}};
    const std::array<int32_t, 4> contentShape{{1, 384, 384, 3}};
    const std::array<int32_t, 4> embeddingShape{{1, 1, 1, 100}};
    if (prediction) {
      require(inputs[0].shape == styleShape && outputs[0].shape == embeddingShape,
              "Unverified prediction model contract");
    } else {
      require(outputs[0].shape == contentShape &&
              ((inputs[0].shape == contentShape && inputs[1].shape == embeddingShape) ||
               (inputs[1].shape == contentShape && inputs[0].shape == embeddingShape)),
              "Unverified transfer model contract");
    }
  }
  std::vector<float> run(const std::vector<float> &pixels, const std::vector<float> &embedding = {}) {
    std::vector<LiteRtTensorBuffer> in, out;
    for (const auto &tensor : inputs) {
      const auto &values = tensor.count == EmbeddingCount ? embedding : pixels;
      require(values.size() == tensor.count, "Input tensor length mismatch");
      void *memory = nullptr;
      check(api.LiteRtLockTensorBuffer(tensor.buffer, &memory, kLiteRtTensorBufferLockModeWrite));
      std::memcpy(memory, values.data(), values.size() * sizeof(float));
      check(api.LiteRtUnlockTensorBuffer(tensor.buffer));
      in.push_back(tensor.buffer);
    }
    for (const auto &tensor : outputs) out.push_back(tensor.buffer);
    check(api.LiteRtRunCompiledModel(compiled, 0, in.size(), in.data(), out.size(), out.data()));
    std::vector<float> result(outputs[0].count);
    void *memory = nullptr;
    check(api.LiteRtLockTensorBuffer(outputs[0].buffer, &memory, kLiteRtTensorBufferLockModeRead));
    std::memcpy(result.data(), memory, result.size() * sizeof(float));
    check(api.LiteRtUnlockTensorBuffer(outputs[0].buffer));
    for (float value : result) require(std::isfinite(value), "Nonfinite inference output");
    return result;
  }
};

uint32_t readWord(bool allowEof = false) {
  std::array<unsigned char, 4> bytes{};
  std::cin.read(reinterpret_cast<char *>(bytes.data()), 4);
  if (allowEof && std::cin.eof() && std::cin.gcount() == 0) return 0;
  require(std::cin.gcount() == 4, "Truncated request header");
  return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
void writeWord(uint32_t value) {
  std::array<char, 4> bytes{{char(value), char(value >> 8), char(value >> 16), char(value >> 24)}};
  std::cout.write(bytes.data(), 4);
}
void reply(uint32_t command, uint32_t sequence, const std::vector<float> &values) {
  writeWord(Magic); writeWord(command); writeWord(sequence); writeWord(uint32_t(values.size()));
  std::cout.write(reinterpret_cast<const char *>(values.data()), std::streamsize(values.size() * sizeof(float)));
  std::cout.flush();
  require(bool(std::cout), "Response pipe closed");
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  MemoryReport memoryReport;
  try {
    require(argc == 4, "Usage: groundzero-style-worker native-library prediction-model transfer-model");
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE float32 required");
    const uint32_t endian = 1;
    require(*reinterpret_cast<const unsigned char *>(&endian) == 1, "Little-endian host required");
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    Api api{std::filesystem::path(argv[1])};
    Environment environment(api);
    Model prediction(api, environment), transfer(api, environment);
    prediction.initialize(std::filesystem::path(argv[2]), true);
    transfer.initialize(std::filesystem::path(argv[3]), false);
    std::vector<float> embedding, previousStyle;
    reply(0, 0, {});  // Ready only after both verified tensor contracts initialize.
    while (true) {
      const auto magic = readWord(true);
      if (!magic && std::cin.eof()) break;
      require(magic == Magic, "Unsupported worker protocol");
      const auto command = readWord(), sequence = readWord(), count = readWord();
      require((command == 1 && count == StyleCount) || (command == 2 && count == ContentCount),
              "Unsupported or oversized request");
      std::vector<float> values(count);
      require(bool(std::cin.read(reinterpret_cast<char *>(values.data()), std::streamsize(count * sizeof(float)))),
              "Truncated request payload");
      for (float value : values)
        require(std::isfinite(value) && value >= 0 && value <= 1, "Input pixels must be finite SDR RGB");
      if (command == 1) {
        if (values != previousStyle) {
          embedding = prediction.run(values);
          previousStyle = std::move(values);
        }
        reply(command, sequence, embedding);
      } else {
        require(embedding.size() == EmbeddingCount, "Capture a reference before styling content");
        reply(command, sequence, transfer.run(values, embedding));
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Groundzero style worker: " << error.what() << '\n';
    return 1;
  }
}
