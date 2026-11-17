#include "toonz/neuralstyleexecution.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <atomic>
#include <future>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Execution = NeuralStyle::WorkerExecution;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "Cannot open fixture file");
  require(file.write(bytes) == bytes.size(), "Cannot write fixture file");
}

QString hash(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct Fixture {
  QTemporaryDir root;
  QByteArray manifest = "{\"fixture\":1}";
  QJsonObject files;
  Fixture() {
    require(root.isValid(), "Cannot create private fixture directory");
    writeFile(root.filePath("package.json"), manifest);
    writeFile(root.filePath("payload"), "test");
    files.insert("worker", QJsonObject{{"file", "payload"},
                                       {"bytes", 4},
                                       {"sha256", hash("test")}});
  }
  std::shared_ptr<Execution> context(int milliseconds = 5000) const {
    return std::make_shared<Execution>(
        root.path(), hash(manifest), files,
        Execution::Clock::now() + std::chrono::milliseconds(milliseconds));
  }
};

void identityChanges() {
  Fixture fixture;
  auto execution = fixture.context();
  QString error;
  require(execution->verify(error), "Unchanged identity must verify");
  writeFile(fixture.root.filePath("payload"), "fake");
  require(!execution->verify(error),
          "Same-size payload replacement must reject");
  writeFile(fixture.root.filePath("payload"), "test");
  writeFile(fixture.root.filePath("package.json"), fixture.manifest + ' ');
  require(!execution->verify(error), "Manifest replacement must reject");
  writeFile(fixture.root.filePath("package.json"), fixture.manifest);
  require(execution->verify(error), "Restored identical bytes must verify");
  require(error.isEmpty(),
          "Successful verification must clear a prior diagnostic");
  writeFile(fixture.root.filePath("payload"), "longer");
  require(!execution->verify(error), "Payload size change must reject");
  std::cout << "PASS expected identity and actual payload changes\n";
}

void declarationsAndCapture() {
  Fixture fixture;
  QString error;
  for (const QString& name :
       {QString("../outside"), QString("payload") + QChar(0)}) {
    fixture.files["worker"] =
        QJsonObject{{"file", name}, {"bytes", 4}, {"sha256", hash("test")}};
    require(!fixture.context()->verify(error),
            "Unsafe declaration must reject before file read");
  }
  require(!NeuralStyle::captureWorkerExecution(
              fixture.root.path(),
              Execution::Clock::now() + std::chrono::seconds(1), error),
          "Unversioned package must not produce a lease");
  QJsonObject manifest{
      {"version", 1}, {"protocol", 1}, {"runtimeVersion", "2.3.0"}};
  writeFile(fixture.root.filePath("package.json"),
            QJsonDocument(manifest).toJson());
  require(!NeuralStyle::captureWorkerExecution(
              fixture.root.path(),
              Execution::Clock::now() + std::chrono::seconds(1), error),
          "Missing pinned models must not produce a lease");
  writeFile(fixture.root.filePath("package.json"), QByteArray(16385, ' '));
  require(!fixture.context()->verify(error), "Oversized manifest must reject");
  std::cout << "PASS unsafe declarations, versions and manifest bounds\n";
}

void deadlineAndCancellation() {
  Fixture fixture;
  auto execution = fixture.context(100);
  auto held      = execution->lockCompute();
  require(held.owns_lock(), "First compute must acquire its buffer lease");
  auto waiter = std::async(std::launch::async, [execution]() {
    const auto started = Execution::Clock::now();
    auto lock          = execution->lockCompute();
    require(!lock.owns_lock(), "Expired waiter must not acquire compute lease");
    return Execution::Clock::now() - started;
  });
  require(waiter.get() < std::chrono::seconds(1),
          "Deadline must bound compute wait");
  QString error;
  require(!execution->verify(error),
          "Expired identity checks must refuse before reading");
  held.unlock();
  const int canceled = 1;
  auto fresh         = fixture.context();
  require(!fresh->lockCompute(&canceled).owns_lock(),
          "Cancellation must refuse compute admission");
  require(fresh->cacheNamespace != execution->cacheNamespace,
          "Different jobs must not share native render aliases");
  std::cout
      << "PASS original deadline and cancellation bound compute waiting\n";
}

void concurrentBuffersAndHelpers() {
  Fixture fixture;
  auto execution = fixture.context();
  std::atomic_int active{0};
  std::vector<std::future<void>> tasks;
  for (int index = 0; index < 8; ++index) {
    tasks.emplace_back(std::async(std::launch::async, [&]() {
      auto lock = execution->lockCompute();
      require(lock.owns_lock(), "Unexpired compute must acquire lease");
      require(++active == 1,
              "Cloned FX must not allocate concurrent model-image staging");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      --active;
    }));
  }
  for (auto& task : tasks) task.get();
  require(execution->helpersClosed(), "New execution must own no helper");
  execution->helperStarting();
  require(!execution->helpersClosed(), "Starting helper must remain owned");
  execution->helperClosed();
  require(execution->helpersClosed(),
          "Only confirmed helper closure releases ownership");
  std::cout << "PASS shared compute serialization and helper ownership\n";
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    identityChanges();
    declarationsAndCapture();
    deadlineAndCancellation();
    concurrentBuffersAndHelpers();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
