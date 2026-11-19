// SPDX-License-Identifier: BSD-3-Clause

// TXshSimpleLevel::load() used to open a modal QMessageBox from library code
// when the first image of a level had five or more samples per pixel, then
// left the level without frames and told its caller nothing. A process with
// nobody to answer the dialog stopped there. load() now records a report on
// the level and passes it to an installed handler, as the editor installs to
// show the message; it opens no dialog itself.
//
// This test writes a sequence of uncompressed TIFF files, one with five
// samples per pixel and a control with four, loads each as a level and checks
// the report, the frames and the handler calls. It has no QApplication, so a
// dialog opened by library code would end the process, and a watchdog ends it
// if a load does not return.

#include "toonz/levelproperties.h"
#include "toonz/preferences.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "tconvert.h"
#include "tenv.h"
#include "texception.h"
#include "tfilepath.h"
#include "tnzimage.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

const int kWatchdogSeconds = 20;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::atomic<bool> g_done{false};

void watch() {
  for (int tenths = 0; tenths < kWatchdogSeconds * 10; ++tenths) {
    if (g_done) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (g_done) return;
  std::cerr << "FAIL: still loading after " << kWatchdogSeconds << " seconds"
            << std::endl;
  std::_Exit(1);
}

void put16(QByteArray& out, int value) {
  out.append(char(value & 0xff));
  out.append(char((value >> 8) & 0xff));
}

void put32(QByteArray& out, int value) {
  put16(out, value & 0xffff);
  put16(out, (value >> 16) & 0xffff);
}

void entry(QByteArray& out, int tag, int type, int count, int value) {
  put16(out, tag);
  put16(out, type);
  put32(out, count);
  if (type == 3 && count == 1) {
    put16(out, value);
    put16(out, 0);
  } else {
    put32(out, value);
  }
}

// A little-endian, uncompressed, 8-bit, interleaved TIFF of mid-grey pixels
// whose first three samples are RGB and the rest are extra samples.
QByteArray makeTiff(int width, int height, int samples) {
  const int entries   = 11;
  const int bitsAt    = 8 + 2 + 12 * entries + 4;
  const int dataAt    = bitsAt + 2 * samples;
  const int dataBytes = width * height * samples;
  QByteArray out;
  out.append("II");
  put16(out, 42);
  put32(out, 8);
  put16(out, entries);
  entry(out, 256, 3, 1, width);
  entry(out, 257, 3, 1, height);
  entry(out, 258, 3, samples, bitsAt);
  entry(out, 259, 3, 1, 1);
  entry(out, 262, 3, 1, 2);
  entry(out, 273, 4, 1, dataAt);
  entry(out, 277, 3, 1, samples);
  entry(out, 278, 3, 1, height);
  entry(out, 279, 4, 1, dataBytes);
  entry(out, 284, 3, 1, 1);
  // Extra samples: the first is unassociated alpha, the others unspecified
  put16(out, 338);
  put16(out, 3);
  put32(out, samples - 3);
  put16(out, 2);
  put16(out, 0);
  put32(out, 0);
  for (int sample = 0; sample < samples; ++sample) put16(out, 8);
  out.append(QByteArray(dataBytes, char(200)));
  return out;
}

void writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              file.write(bytes) == bytes.size(),
          "cannot write " + path.toStdString());
}

struct Handled {
  int calls = 0;
  QString title, message;
};
Handled g_handled;

void handler(const QString& title, const QString& message) {
  ++g_handled.calls;
  g_handled.title   = title;
  g_handled.message = message;
}

TXshSimpleLevel* makeLevel(ToonzScene& scene, const QString& dir,
                           const QString& name, int samples) {
  for (int frame = 1; frame <= 2; ++frame)
    writeFile(QDir(dir).filePath(
                  QString("%1.%2.tif").arg(name).arg(frame, 4, 10, QChar('0'))),
              makeTiff(2, 2, samples));
  auto* level = new TXshSimpleLevel(name.toStdWString());
  level->setScene(&scene);
  level->setType(OVL_XSHLEVEL);
  level->setPath(TFilePath(QDir(dir).filePath(name + "..tif").toStdWString()),
                 true);
  return level;
}

void fiveSamples(ToonzScene& scene, const QString& dir) {
  TXshSimpleLevel::setLoadReportHandler(handler);
  g_handled              = Handled();
  TXshSimpleLevel* level = makeLevel(scene, dir, "five", 5);
  level->load();
  const TXshSimpleLevel::LoadReport& report = level->getLoadReport();
  require(report.m_failed, "the five-sample level reported no failure");
  require(report.m_title == "Image format not supported" &&
              report.m_message.contains("Samples per pixel is more than 4"),
          "the report does not say why: " + report.m_message.toStdString());
  require(report.m_message.contains("five..tif"),
          "the report does not name the level path: " +
              report.m_message.toStdString());
  require(level->getFrameCount() == 0,
          "the five-sample level has frames after the failed load");
  require(g_handled.calls == 1 && g_handled.message == report.m_message &&
              g_handled.title == report.m_title,
          "the handler was not given the report once");
  level->load();
  require(g_handled.calls == 2 && level->getLoadReport().m_failed,
          "a second load did not report the failure again");

  TXshSimpleLevel::setLoadReportHandler(nullptr);
  level->load();
  require(g_handled.calls == 2 && level->getLoadReport().m_failed,
          "without a handler the failure is not left on the level");
  std::cout << "  five samples per pixel: reported, no frames, no dialog"
            << std::endl;
  level->release();
}

void fourSamples(ToonzScene& scene, const QString& dir) {
  TXshSimpleLevel::setLoadReportHandler(handler);
  g_handled              = Handled();
  TXshSimpleLevel* level = makeLevel(scene, dir, "four", 4);
  level->load();
  require(level->getLoadReport().isEmpty() && g_handled.calls == 0,
          "a four-sample level reported a failure: " +
              level->getLoadReport().m_message.toStdString());
  require(level->getFrameCount() == 2,
          "the four-sample level did not load its two frames");
  TXshSimpleLevel::setLoadReportHandler(nullptr);
  std::cout << "  four samples per pixel: loaded, nothing reported"
            << std::endl;
  level->release();
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  std::thread watchdog(watch);
  int failures = 0;
  try {
    QTemporaryDir storage;
    require(storage.isValid(), "cannot create a temporary directory");
    const QString stuff = QDir(storage.path()).filePath("stuff");
    require(
        QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default")),
        "cannot create the stuff directory");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    initImageIo();
    ToonzScene scene;
    TProjectManager::instance()->initializeScene(&scene);
    fiveSamples(scene, storage.path());
    fourSamples(scene, storage.path());
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    ++failures;
  } catch (const TException& error) {
    std::cerr << "FAIL: " << ::to_string(error.getMessage()) << std::endl;
    ++failures;
  }
  g_done = true;
  watchdog.join();
  std::cout << (failures ? "level_load_report: FAILED"
                         : "level_load_report: PASS")
            << std::endl;
  return failures ? 1 : 0;
}
