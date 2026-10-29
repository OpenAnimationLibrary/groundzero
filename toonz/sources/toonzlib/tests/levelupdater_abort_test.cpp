#include "toonz/levelupdater.h"
#include "tenv.h"
#include "texception.h"
#include "tproperty.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "cannot open fixture output");
  require(file.write(bytes) == bytes.size(), "cannot write fixture output");
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "cannot read fixture output");
  return file.readAll();
}

struct Probe {
  bool failFrame = false;
  bool failFinalize = false;
  int finalizeCalls = 0;
  int abortCalls = 0;
  int destructorFinalizations = 0;
  int destroyed = 0;
};

Probe *currentProbe = nullptr;

class ProbeFrameWriter final : public TImageWriter {
  Probe &m_probe;
public:
  ProbeFrameWriter(const TFilePath &path, Probe &probe)
      : TImageWriter(path), m_probe(probe) {}
  void save(const TImageP &) override {
    if (m_probe.failFrame) throw TException("Injected frame failure");
  }
};

// Model a staged encoder with legacy destructor finalization. The regression
// must exercise LevelUpdater's real ownership/destruction, not just a flag in
// a separate state helper: releasing this writer without abort publishes data.
class ProbeLevelWriter final : public TLevelWriter {
  Probe &m_probe;
  bool m_done = false;
public:
  ProbeLevelWriter(const TFilePath &path, TPropertyGroup *properties)
      : TLevelWriter(path, properties), m_probe(*currentProbe) {}
  ~ProbeLevelWriter() override {
    if (!m_done) {
      ++m_probe.destructorFinalizations;
      try { finalize(); } catch (...) {}
    }
    ++m_probe.destroyed;
  }
  TImageWriterP getFrameWriter(TFrameId) override {
    return new ProbeFrameWriter(m_path, m_probe);
  }
  void finalize() override {
    if (m_done) return;
    ++m_probe.finalizeCalls;
    if (m_probe.failFinalize) throw TException("Injected encoder failure");
    writeFile(m_path.getQString(), "replacement");
    m_done = true;
  }
  void abort() noexcept override {
    if (m_done) return;
    ++m_probe.abortCalls;
    m_done = true;
  }
};

TLevelWriter *createProbeWriter(const TFilePath &path,
                               TPropertyGroup *properties) {
  return new ProbeLevelWriter(path, properties);
}

void checkLifecycle(const QString &path, bool cancel, bool frameFailure,
                    bool encoderFailure) {
  Probe probe;
  currentProbe = &probe;
  writeFile(path, "previous");
  {
    TPropertyGroup properties;
    LevelUpdater updater(TFilePath(path), &properties,
                         TFrameId(TFrameId::NO_FRAME), true);
    updater.update(TFrameId(1), TImageP());
    if (frameFailure) {
      probe.failFrame = true;
      bool rejected = false;
      try { updater.update(TFrameId(2), TImageP()); }
      catch (const TException &) { rejected = true; }
      require(rejected, "fixture did not inject the frame failure");
    }
    if (cancel || frameFailure) {
      updater.abort();
      updater.abort();
      updater.close();
    } else {
      probe.failFinalize = encoderFailure;
      bool rejected = false;
      try { updater.close(); }
      catch (const TException &) { rejected = true; }
      require(rejected == encoderFailure, "close lost encoder failure status");
    }
    require(!updater.opened(), "terminal cleanup left the updater open");
  }
  const bool discard = cancel || frameFailure || encoderFailure;
  require(readFile(path) == (discard ? "previous" : "replacement"),
          "failed or canceled update replaced the prior destination");
  require(probe.abortCalls == (discard ? 1 : 0), "abort was lost or repeated");
  require(probe.finalizeCalls == (cancel || frameFailure ? 0 : 1),
          "discarded output was finalized or close was repeated");
  require(probe.destructorFinalizations == 0 && probe.destroyed == 1,
          "destructor retried publication or leaked the writer");
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir storage;
    require(storage.isValid(), "cannot create test storage");
    QDir dir(storage.path());
    const QString stuff = dir.filePath("stuff");
    QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuff));
    TEnv::setApplicationFullName("levelupdater_abort_test");
    TLevelWriter::define("abortprobe", createProbeWriter, false);
    const QString path = dir.filePath("output.abortprobe");
    checkLifecycle(path, true, false, false);
    checkLifecycle(path, false, true, false);
    checkLifecycle(path, false, false, true);
    checkLifecycle(path, false, false, false);
    LevelUpdater unopened;
    unopened.abort();
    unopened.close();
    std::cout << "level updater abort and publication lifecycle passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Unexpected updater exception\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
