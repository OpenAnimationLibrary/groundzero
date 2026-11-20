#include "toonz/neuralstyle.h"
#include "toonz/neuralstyleexecution.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using Execution = NeuralStyle::WorkerExecution;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
QImage pixels(int side) {
  QImage result(side, side, QImage::Format_RGB32);
  result.fill(qRgb(30, 140, 90));
  return result;
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 3) {
    std::cerr << "Two distinct staged packages are required; the second is "
                 "mutated.\n";
    return 77;
  }
  if (QDir(QString::fromUtf8(argv[1])).canonicalPath() ==
      QDir(QString::fromUtf8(argv[2])).canonicalPath())
    return 2;
  try {
    QString error;
    const auto deadline = Execution::Clock::now() + std::chrono::seconds(20);
    auto first = NeuralStyle::captureWorkerExecution(QString::fromUtf8(argv[1]),
                                                     deadline, error);
    auto second = NeuralStyle::captureWorkerExecution(
        QString::fromUtf8(argv[2]), deadline, error);
    require(bool(first) && bool(second), qPrintable(error));
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto owner    = std::async(std::launch::async, [&] {
      NeuralStyle::Session session(first->directory, first);
      QString diagnostic;
      std::vector<QImage> result;
      const bool ok = session.styleBatch(
          pixels(256), {pixels(384)}, result, diagnostic, {},
          [&](int frame, int) {
            if (frame == 0) {
              entered.set_value();
              require(released.wait_for(std::chrono::seconds(10)) ==
                          std::future_status::ready,
                      "Test controller did not release the held worker lease");
            }
          });
      require(ok, qPrintable(diagnostic));
    });
    require(entered.get_future().wait_for(std::chrono::seconds(5)) ==
                std::future_status::ready,
            "Owner did not hold actual native worker lease");
    auto waiting = std::async(std::launch::async, [&] {
      NeuralStyle::Session session(second->directory, second);
      QString diagnostic;
      QImage sentinel = pixels(3), result = sentinel;
      const bool ok =
          session.style(pixels(256), pixels(384), result, diagnostic);
      require(!ok,
              "Queued changed package unexpectedly launched and succeeded");
      require(diagnostic ==
                  "Neural package identity changed after preview admission.",
              "Queued rejection was not the expected-identity check");
      require(result == sentinel, "Queued rejection replaced caller output");
    });
    require(waiting.wait_for(std::chrono::milliseconds(250)) ==
                std::future_status::timeout,
            "Second Session did not wait for actual native lease");
    QFile manifest(QDir(second->directory).filePath("package.json"));
    require(manifest.open(QIODevice::Append),
            "Cannot change private waiting package");
    require(manifest.write(" ") == 1, "Cannot update private manifest");
    manifest.close();
    release.set_value();
    owner.get();
    waiting.get();
    require(first->helpersClosed() && second->helpersClosed(),
            "Sessions retained helper ownership");
    std::cout << "PASS changed expected identity rejected after FIFO "
                 "acquisition with no publication\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
