#include "nativediagnosticswriter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace NativeDiagnostic;
namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
QByteArray read(const QString &fileName) {
  QFile file(fileName);
  require(file.open(QIODevice::ReadOnly), "read diagnostic output");
  return file.readAll();
}
void idle(Writer &writer) {
  require(writer.waitUntilIdle(15000), "diagnostic queue drains within test deadline");
}
Record message(const QString &text, Severity severity = Severity::Info) {
  Record result;
  result.timestamp = QDateTime::fromString("2020-01-02T03:04:05.006Z", Qt::ISODateWithMs);
  result.source = "test";
  result.text = text;
  result.severity = severity;
  return result;
}
void run() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary directory available");
  const QString directory = temporary.path() + QString::fromUtf8("/diagnostics-\xe7\xbb\x98\xe5\x9b\xbe");
  const QString file = directory + "/diagnostics.log";
  Writer writer;
  writer.append(message("disabled"));
  require(!QFileInfo::exists(directory), "off does not create a directory or worker log");
  writer.enable(directory);
  idle(writer);
  require(writer.status().state == State::Ready, "Unicode directory is writable");
  writer.append(message("once"));
  writer.append(message("ignored-debug", Severity::Debug));
  idle(writer);
  QByteArray output = read(file);
  require(output.count("once") == 1, "event written once");
  require(!output.contains("ignored-debug"), "minimum severity enforced");
  require(output.contains("2020-01-02T03:04:05.006Z"), "capture timestamp retained");
  writer.append(message("line one\nforged prefix\rnext"));
  idle(writer);
  output = read(file);
  require(output.contains("line one\\nforged prefix\\rnext"), "multiline controls escaped in one event");

  Writer competing;
  competing.enable(directory);
  idle(competing);
  require(competing.status().state == State::Locked, "second instance cannot rotate active logs");
  competing.append(message("blocked"));
  require(competing.status().dropped == 1, "unavailable destination accounts for dropped events");
  competing.removeSavedLogs(directory);
  idle(competing);
  require(QFileInfo::exists(file), "competing delete leaves active logs intact");
  writer.disable();
  idle(writer);
  competing.enable(directory);
  idle(competing);
  require(competing.status().state == State::Ready, "ownership released on disable and retry succeeds");
  competing.append(message("after-retry"));
  idle(competing);
  require(read(file).contains("after-retry"), "reopened log remains functional");
  competing.disable();
  idle(competing);
  QFile unrelated(directory + "/keep.txt");
  require(unrelated.open(QIODevice::WriteOnly), "unrelated fixture created");
  unrelated.write("keep");
  unrelated.close();
  competing.removeSavedLogs(directory);
  idle(competing);
  require(!QFileInfo::exists(file), "owned log deleted while disabled");
  require(read(unrelated.fileName()) == "keep", "deletion touches only owned filenames");

  const QString obstacle = temporary.path() + "/not-a-directory";
  QFile blocked(obstacle);
  require(blocked.open(QIODevice::WriteOnly), "invalid directory fixture created");
  blocked.write("unchanged"); blocked.close();
  writer.enable(obstacle);
  idle(writer);
  require(writer.status().state == State::Error, "startup failure is recoverable status");
  require(read(obstacle) == "unchanged", "failure does not overwrite obstructing file");
  writer.enable(directory);
  idle(writer);
  require(writer.status().state == State::Ready, "retry recovers after failure");
  writer.shutdown();
  competing.shutdown();

  Limits limits;
  limits.fileBytes = 1024;
  limits.recordBytes = 900;
  limits.queueBytes = 1800;
  limits.queueRecords = 2;
  const QString rotatedDirectory = temporary.path() + "/rotation";
  {
    Writer rotating(limits);
    rotating.enable(rotatedDirectory);
    idle(rotating);
    for (int index = 0; index < 12; ++index) {
      rotating.append(message(QString("event-%1 ").arg(index) + QString(1400, 'x')));
      idle(rotating);
    }
    require(rotating.status().written == 12, "oversized entries are retained with explicit truncation");
    rotating.shutdown();
  }
  QDir rotated(rotatedDirectory);
  QStringList files = rotated.entryList({"diagnostics*.log"}, QDir::Files);
  require(files.size() == 4, "active file and three backups only");
  for (const QString &name : files) {
    require(QFileInfo(rotated.filePath(name)).size() <= 1024, "rotation respects file size bound");
    require(read(rotated.filePath(name)).contains("[truncated]"), "truncation disclosed");
  }
  {
    Writer restarted(limits);
    restarted.enable(rotatedDirectory); idle(restarted);
    restarted.append(message("new-launch")); idle(restarted);
    restarted.shutdown();
  }
  require(rotated.entryList({"diagnostics*.log"}, QDir::Files).size() == 4,
          "restart reuses bounded file family");

  // Force an actual sink failure after startup: the next rotation cannot
  // replace a nonempty directory occupying its last backup destination.
  const QString failureDirectory = temporary.path() + "/rotation-failure";
  Writer failing(limits);
  failing.enable(failureDirectory); idle(failing);
  failing.append(message(QString(1200, 'x'))); idle(failing);
  const QString rotationObstacle = failureDirectory + "/diagnostics.3.log";
  require(QDir().mkdir(rotationObstacle),
          "rotation obstacle created after opening the sink");
  QFile protectedFile(rotationObstacle + "/keep.txt");
  require(protectedFile.open(QIODevice::WriteOnly), "rotation obstacle is nonempty");
  protectedFile.write("keep"); protectedFile.close();
  QFile previousBackup(failureDirectory + "/diagnostics.2.log");
  require(previousBackup.open(QIODevice::WriteOnly), "previous backup fixture created");
  previousBackup.write("previous"); previousBackup.close();
  failing.append(message(QString(1200, 'y'))); idle(failing);
  require(failing.status().state == State::Error && failing.status().dropped == 1,
          "runtime sink failure becomes status and accounts for unwritten event");
  require(read(protectedFile.fileName()) == "keep",
          "rotation obstacle remains untouched");
  require(protectedFile.remove() && QDir().rmdir(rotationObstacle),
          "runtime obstruction removed for retry");
  failing.enable(failureDirectory); idle(failing);
  require(failing.status().state == State::Ready, "sink can reopen after runtime failure");
  failing.shutdown();

  Writer flooded(limits);
  flooded.enable(temporary.path() + "/flood"); idle(flooded);
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 8; ++thread)
    threads.emplace_back([&flooded, thread] {
      for (int index = 0; index < 1500; ++index)
        flooded.append(message(QString("%1:%2 ").arg(thread).arg(index) + QString(200, 'z')));
    });
  for (auto &thread : threads) thread.join();
  auto pending = flooded.status();
  require(pending.queued <= limits.queueRecords && pending.queuedBytes <= limits.queueBytes,
          "concurrent producer queue remains bounded");
  idle(flooded);
  const auto complete = flooded.status();
  require(complete.written + complete.dropped == 12000, "every submitted event is written or counted as dropped");
  require(complete.queued == 0 && complete.queuedBytes == 0, "queue accounting empties after draining");
  for (int change = 0; change < 10000; ++change) {
    if (change % 2) flooded.enable(temporary.path() + "/flood");
    else flooded.disable();
    require(flooded.status().pendingChanges <= 2,
            "control requests cannot grow the queue without bound");
  }
  flooded.disable();
  idle(flooded);
  require(flooded.status().state == State::Off,
          "rapid control changes coalesce and finish in the latest requested state");
  flooded.shutdown();
}
}  // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try { run(); std::cout << "native diagnostic writer passed\n"; return 0; }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
