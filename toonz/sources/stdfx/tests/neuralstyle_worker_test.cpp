#include "neuralstylefx.cpp"
#include "neuraltemporal.cpp"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

namespace {
using Execution = NeuralStyle::WorkerExecution;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  QTemporaryDir root;
  NeuralStyle::Profile profile;
  Fixture() {
    require(root.isValid(), "Cannot create private fixture directory");
    QFile file(root.filePath("package.json"));
    require(file.open(QIODevice::WriteOnly), "Cannot create fixture manifest");
    require(file.write("{}") == 2, "Cannot write fixture manifest");
    profile.reference = QImage(256, 256, QImage::Format_RGB32);
    profile.reference.fill(qRgb(150, 30, 90));
  }
  std::shared_ptr<const Execution> execution(int milliseconds = 5000) const {
    return std::make_shared<Execution>(
        root.path(),
        QString::fromLatin1(
            QCryptographicHash::hash("{}", QCryptographicHash::Sha256).toHex()),
        QJsonObject(),
        Execution::Clock::now() + std::chrono::milliseconds(milliseconds));
  }
  TFxP effect(const std::shared_ptr<const Execution>& context) const {
    return NeuralStyle::createWorkerPreviewFx(profile, 35, 0, context);
  }
};

class Source final : public TStandardRasterFx {
public:
  const TPersistDeclaration* getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings&, double) override { return true; }
  bool doGetBBox(double, TRectD& box, const TRenderSettings&) override {
    box = TRectD(0, 0, 64, 64);
    return true;
  }
  std::string getAlias(double, const TRenderSettings&) const override {
    return "worker-test-source";
  }
  void compute(TTile& tile, double, const TRenderSettings&) override {
    const TRaster32P raster = tile.getRaster();
    require(bool(raster), "Fixture source requires RGBA8");
    raster->fill(TPixel32(40, 120, 80, 255));
  }
  void doCompute(TTile& tile, double frame,
                 const TRenderSettings& info) override {
    compute(tile, frame, info);
  }
};

TRenderSettings settings() {
  TRenderSettings result;
  result.m_bpp              = 32;
  result.m_cameraBox        = TRectD(0, 0, 64, 64);
  result.m_linearColorSpace = false;
  return result;
}

void cloneIdentityAndExpiry() {
  Fixture fixture;
  const auto context  = fixture.execution();
  const TFxP original = fixture.effect(context);
  const TFxP clone    = original->clone(false);
  require(original->getAlias(0, settings()) == clone->getAlias(0, settings()),
          "Actual native clone lost its execution alias identity");
  const TFxP independent = fixture.effect(fixture.execution());
  require(
      original->getAlias(0, settings()) != independent->getAlias(0, settings()),
      "Independent jobs reused native rendered-image aliases");
  const TFxP expired      = fixture.effect(fixture.execution(-1));
  const TFxP expiredClone = expired->clone(false);
  TTile tile(TRaster32P(64, 64), TPointD());
  bool rejected = false;
  try {
    static_cast<NeuralStyleFx*>(expiredClone.getPointer())
        ->doCompute(tile, 0, settings());
  } catch (const TException&) {
    rejected = true;
  }
  require(rejected, "Actual cloned FX bypassed its original deadline");
  std::cout
      << "PASS actual native clone retains identity and original deadline\n";
}

void precisionAndMaskAdmission() {
  Fixture fixture;
  const TFxP effect = fixture.effect(fixture.execution());
  auto* native      = static_cast<NeuralStyleFx*>(effect.getPointer());
  TTile highPrecision(TRaster64P(64, 64), TPointD());
  bool rejected = false;
  try {
    native->doCompute(highPrecision, 0, settings());
  } catch (const TException&) {
    rejected = true;
  }
  require(rejected, "Worker image budget must refuse high-precision tiles");
  const TFxP mask = new Source;
  effect->getInputPort("Mask")->setFx(mask.getPointer());
  TTile rgba(TRaster32P(64, 64), TPointD());
  rejected = false;
  try {
    native->doCompute(rgba, 0, settings());
  } catch (const TException&) {
    rejected = true;
  }
  require(rejected, "Worker image budget must refuse connected mask buffers");
  std::cout
      << "PASS worker precision and mask admission preserve image budget\n";
}

void clonedComputeSerialization() {
  Fixture fixture;
  const TFxP effect = fixture.effect(fixture.execution());
  const TFxP source = new Source;
  effect->getInputPort("Source")->setFx(source.getPointer());
  std::vector<TFxP> clones;
  for (int index = 0; index < 8; ++index) {
    TFxP clone = effect->clone(false);
    clone->getInputPort("Source")->setFx(source.getPointer());
    clones.push_back(clone);
  }
  cache.clear();
  std::atomic_int active{0}, maximum{0}, calls{0};
  inferenceForTests = [&](const QImage&, const std::vector<QImage>& inputs,
                          std::vector<QImage>& outputs, QString&) {
    const int current = ++active;
    int before        = maximum.load();
    while (before < current &&
           !maximum.compare_exchange_weak(before, current)) {
    }
    ++calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    outputs = inputs;
    --active;
    return true;
  };
  std::promise<void> start;
  auto started = start.get_future().share();
  std::vector<std::future<void>> tasks;
  for (const auto& clone : clones) {
    tasks.emplace_back(std::async(std::launch::async, [clone, started] {
      started.wait();
      TTile tile(TRaster32P(64, 64), TPointD());
      static_cast<NeuralStyleFx*>(clone.getPointer())
          ->doCompute(tile, 0, settings());
      const TRaster32P pixels = tile.getRaster();
      require(pixels->pixels(0)[0].m == 255,
              "Native clone did not preserve alpha");
    }));
  }
  start.set_value();
  for (auto& task : tasks) task.get();
  inferenceForTests = {};
  require(calls > 0 && maximum == 1,
          "Cloned FX allocated concurrent inference staging");
  std::cout << "PASS actual eight cloned FX serialize image staging and "
               "preserve alpha\n";
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    cloneIdentityAndExpiry();
    precisionAndMaskAdmission();
    clonedComputeSerialization();
    return 0;
  } catch (const TException& error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
