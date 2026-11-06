#include "toonz/mediacache.h"
#include <QCoreApplication>
#include <QProcess>
#include <QSemaphore>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char *reason) {
  if (!value) throw std::runtime_error(reason);
}
QString generation(const QString &root, QChar letter, int bytes = 4096) {
  QString base = QDir(root).filePath("import-v2-" + QString(64, letter));
  require(QDir().mkpath(base + ".frames"), "cannot create fixture generation");
  QFile frame(QDir(base + ".frames").filePath("frame000001.png"));
  require(frame.open(QIODevice::WriteOnly), "cannot write fixture frame");
  require(frame.write(QByteArray(bytes, 'x')) == bytes, "fixture frame write failed");
  return base;
}
void startReader(QProcess &process, const QString &base) {
  process.start(QCoreApplication::applicationFilePath(), {"--reader", base});
  require(process.waitForStarted(5000), "child reader did not start");
  require(process.waitForReadyRead(5000), "child lease not acquired");
  require(process.readAllStandardOutput().contains("leased"), "child lease failed");
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (app.arguments().size() == 3 && app.arguments()[1] == "--reader") {
      MediaCache::Lease lease(app.arguments()[2]);
      std::cout << "leased\n" << std::flush;
      std::string line;
      std::getline(std::cin, line);
      return 0;
    }
    QTemporaryDir temporary;
    require(temporary.isValid(), "fixture directory unavailable");
    const QString root = temporary.path();
    const QString a = generation(root, 'a');
    const QString b = generation(root, 'b', 8192);
    QFile source(QDir(root).filePath("original-movie.mp4"));
    require(source.open(QIODevice::WriteOnly), "fixture movie unavailable");
    source.write("original"); source.close();
    QFile exportFile(QDir(root).filePath("export_fixture_in000001.png"));
    require(exportFile.open(QIODevice::WriteOnly), "fixture export unavailable");
    exportFile.write("active export"); exportFile.close();
    {
      MediaCache::Lease first(a), second(a);
      auto report = MediaCache::usage(root);
      require(report.generations == 2 && report.activeGenerations == 1,
              "reader identity accounting incorrect");
      require(report.bytes == 12288 && report.activeBytes == 4096,
              "cache byte accounting incorrect");
      report = MediaCache::trim(root, 0);
      require(report.errors.isEmpty() && report.removedGenerations == 1,
              "unused generation was not retired");
      require(QFileInfo::exists(a + ".frames") && !QFileInfo::exists(b + ".frames"),
              "cleanup removed active reader or retained unused generation");
      require(MediaCache::trim(root, 0).removedGenerations == 0,
              "repeated cleanup ignored active readers");
    }
    require(MediaCache::trim(root, 0).removedGenerations == 1,
            "released generation was not eligible");
    require(QFileInfo::exists(source.fileName()) && QFileInfo::exists(exportFile.fileName()),
            "cleanup touched source/export files");
    generation(root, 'c');
    const QString c = QDir(root).filePath("import-v2-" + QString(64, 'c'));
    {
      QProcess reader;
      startReader(reader, c);
      require(MediaCache::trim(root, 0).removedGenerations == 0,
              "cross-process live reader was evicted");
      reader.write("done\n"); reader.closeWriteChannel();
      require(reader.waitForFinished(5000) && reader.exitCode() == 0,
              "child reader failed to release");
    }
    require(MediaCache::trim(root, 0).removedGenerations == 1,
            "cross-process released reader remained locked");
    generation(root, 'd');
    const QString d = QDir(root).filePath("import-v2-" + QString(64, 'd'));
    {
      QProcess reader;
      startReader(reader, d);
      reader.kill();
      require(reader.waitForFinished(5000), "child reader termination failed");
    }
    require(MediaCache::trim(root, 0).removedGenerations == 1,
            "dead reader lease was not recovered");
    const QString e = generation(root, 'e');
    std::atomic<bool> cancel{true};
    const auto canceled = MediaCache::trim(root, 0, &cancel);
    require(canceled.canceled && QFileInfo::exists(e + ".frames"),
            "canceled cleanup removed pending generations");
    // Cancellation can arrive after the outer loop check while registration
    // owns the management gate. It must be observed before atomic retirement.
    QSemaphore cancelInventory, allowRetirement, retirementGate;
    MediaCache::testHooks().afterInventory = [&]() {
      cancelInventory.release();
      allowRetirement.acquire();
    };
    MediaCache::testHooks().onManagementGateContention = [&]() {
      retirementGate.release();
    };
    cancel.store(false);
    MediaCache::Report canceledAtGate;
    std::thread blockedCleaner([&]() { canceledAtGate = MediaCache::trim(root, 0, &cancel); });
    const bool readyForGate = cancelInventory.tryAcquire(1, 5000);
    QLockFile registration(QDir(root).filePath(".manage.lock"));
    registration.setStaleLockTime(0);
    const bool registrationHeld = readyForGate && registration.tryLock(0);
    allowRetirement.release();
    const bool reachedRetirementGate = registrationHeld && retirementGate.tryAcquire(1, 5000);
    cancel.store(true);
    if (registrationHeld) registration.unlock();
    blockedCleaner.join();
    MediaCache::testHooks().afterInventory = {};
    MediaCache::testHooks().onManagementGateContention = {};
    require(readyForGate && registrationHeld && reachedRetirementGate,
            "cancellation fixture did not reach the locked retirement boundary");
    require(canceledAtGate.canceled && canceledAtGate.errors.isEmpty() &&
                canceledAtGate.removedGenerations == 0 && QFileInfo::exists(e + ".frames"),
            "cleanup retired a generation after cancellation while waiting for its gate");
    const QString retired = QDir(root).filePath(".retired-import-v2-" +
        QString(64, 'e') + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    require(QDir().rename(e + ".frames", retired), "cannot stage interrupted retirement");
    require(MediaCache::usage(root).bytes == 4096, "retired data missing from usage");
    require(MediaCache::trim(root, 0).removedGenerations == 1 && !QFileInfo::exists(retired),
            "interrupted retirement was not resumed");
    QString error;
    require(MediaCache::setBudget(root, 2048, &error) && MediaCache::budget(root) == 2048,
            "budget failed to persist");
    generation(root, 'f');
    require(MediaCache::enforceBudget(root).removedGenerations == 1,
            "automatic configured budget was not enforced");
    generation(root, 'f');
    MediaCache::scheduleBudget(root);
    MediaCache::scheduleBudget(root);
    require(QThreadPool::globalInstance()->waitForDone(5000) &&
                MediaCache::usage(root).generations == 0,
            "coalesced background budget cleanup did not finish");
    require(MediaCache::setBudget(root, 0, &error), "cannot disable automatic budget");
    const QString retained = generation(root, 'f');
    require(MediaCache::enforceBudget(root).removedGenerations == 0 &&
                QFileInfo::exists(retained + ".frames"), "disabled budget removed data");
    MediaCache::trim(root, 0);

    // Two cleaners must not both budget against a stale copy of the same
    // inventory. Hold the first immediately after inventory, then start a
    // second against the same two generations and budget.
    generation(root, 'a'); generation(root, 'b');
    QSemaphore inventoried, proceed, secondStarted;
    std::atomic<bool> firstPass{true};
    MediaCache::testHooks().afterInventory = [&]() {
      if (firstPass.exchange(false)) { inventoried.release(); proceed.acquire(); }
    };
    MediaCache::Report one, two;
    std::thread firstCleaner([&]() { one = MediaCache::trim(root, 4096); });
    if (!inventoried.tryAcquire(1, 5000)) {
      proceed.release(); firstCleaner.join();
      throw std::runtime_error("first cleaner did not reach inventory");
    }
    QLockFile cleanupProbe(QDir(root).filePath(".cleanup.lock"));
    cleanupProbe.setStaleLockTime(0);
    const bool serialized = !cleanupProbe.tryLock(0);
    std::thread secondCleaner([&]() { secondStarted.release(); two = MediaCache::trim(root, 4096); });
    secondStarted.acquire(); proceed.release();
    firstCleaner.join(); secondCleaner.join();
    MediaCache::testHooks().afterInventory = {};
    require(serialized && one.errors.isEmpty() && two.errors.isEmpty() &&
                MediaCache::usage(root).generations == 1,
            "concurrent cleaners over-evicted a shared budget");
    MediaCache::trim(root, 0);

    // Close the last reader precisely after a background pass observed its
    // lease. A queued request must cause another pass, not be dropped.
    const QString late = generation(root, 'c');
    auto lateReader = std::make_unique<MediaCache::Lease>(late);
    require(MediaCache::setBudget(root, 2048, &error), "cannot enable interleaving budget");
    QSemaphore protectedSeen, finishPass;
    std::atomic<bool> observed{false};
    MediaCache::testHooks().afterProtectedGeneration = [&]() {
      if (!observed.exchange(true)) { protectedSeen.release(); finishPass.acquire(); }
    };
    MediaCache::scheduleBudget(root);
    if (!protectedSeen.tryAcquire(1, 5000)) {
      finishPass.release(); QThreadPool::globalInstance()->waitForDone();
      throw std::runtime_error("background cleaner did not observe the open reader");
    }
    lateReader.reset();
    MediaCache::scheduleBudget(root);
    finishPass.release();
    const bool finished = QThreadPool::globalInstance()->waitForDone(5000);
    if (!finished) QThreadPool::globalInstance()->waitForDone();
    MediaCache::testHooks().afterProtectedGeneration = {};
    require(finished && MediaCache::usage(root).generations == 0,
            "background budget lost the final reader-close request");
    const QString incomplete = generation(root, 'd');
    const QString partial = incomplete + ".partial-Ab12Cd";
    require(QDir().rename(incomplete + ".frames", partial), "cannot stage partial extraction");
    {
      QLockFile extractor(incomplete + ".lock");
      extractor.setStaleLockTime(0);
      require(extractor.tryLock(0), "cannot hold native extraction lock");
      const auto report = MediaCache::usage(root);
      require(report.bytes == 4096 && report.activeBytes == 4096,
              "partial extraction bytes were hidden from usage");
      require(MediaCache::trim(root, 0).removedGenerations == 0 && QFileInfo::exists(partial),
              "active partial extraction was removed");
    }
    require(MediaCache::trim(root, 0).removedGenerations == 1 && !QFileInfo::exists(partial),
            "abandoned partial extraction could not be cleaned");
    std::cout << "Media cache leases, cancellation, retirement and budgets passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
