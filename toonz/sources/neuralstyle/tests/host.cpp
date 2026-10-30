#include "toonz/neuralstyle.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <chrono>
#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
QByteArray read(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot open fixture");
  return file.readAll();
}
void write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
          file.write(bytes) == bytes.size(), "Cannot write fixture");
}
QJsonObject entry(const QString &path) {
  const auto bytes = read(path);
  return {{"file", QFileInfo(path).fileName()}, {"bytes", bytes.size()},
          {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}
QImage image(const QString &path, int side) {
  const auto bytes = read(path);
  require(bytes.size() == side * side * 3, "Wrong fixture dimensions");
  return QImage(reinterpret_cast<const uchar *>(bytes.constData()), side, side,
                side * 3, QImage::Format_RGB888).copy();
}
void compare(const QImage &actual, const QImage &expected) {
  require(actual.size() == expected.size(), "Output dimensions changed");
  for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x) {
    const auto a = actual.pixel(x, y), b = expected.pixel(x, y);
    require(std::abs(qRed(a)-qRed(b)) <= 1 && std::abs(qGreen(a)-qGreen(b)) <= 1 &&
            std::abs(qBlue(a)-qBlue(b)) <= 1 && qAlpha(a) == 255,
            "Native host differs from Python oracle");
  }
}
void reply(quint32 command, quint32 sequence, quint32 count, bool fragmented) {
  quint32 header[] = {qToLittleEndian(quint32(0x31535a47)), qToLittleEndian(command),
                      qToLittleEndian(sequence), qToLittleEndian(count)};
  const auto bytes = reinterpret_cast<const char *>(header);
  if (fragmented) {
    for (int i = 0; i < 16; ++i) {
      std::cout.write(bytes+i, 1).flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } else std::cout.write(bytes, 16);
  const QByteArray payload(int(count)*4, 0);
  std::cout.write(payload.constData(), payload.size()).flush();
}
int fakeWorker(const QString &runtime) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  const auto mode = read(runtime);
  reply(0, 0, 0, true);
  for (;;) {
    quint32 header[4];
    if (!std::cin.read(reinterpret_cast<char *>(header), 16)) return 0;
    const auto command = qFromLittleEndian(header[1]);
    const auto sequence = qFromLittleEndian(header[2]);
    const auto count = qFromLittleEndian(header[3]);
    if (count > 384*384*3) return 2;
    QByteArray payload(int(count)*4, 0);
    if (!std::cin.read(payload.data(), payload.size())) return 3;
    if (mode == "crash") return 4;
    if (mode == "stall") { std::this_thread::sleep_for(std::chrono::seconds(35)); return 5; }
    if (mode == "wrong") { reply(99, sequence, 0, false); return 0; }
    if (mode == "oversized") { reply(command, sequence, 384*384*3+1, false); return 0; }
    if (mode == "slow-batch") {
      write(runtime+".ready", "ready");
      if(command==2)std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::cerr << std::string(1024*1024, 'x');
    reply(command, sequence, command == 1 ? 100 : 384*384*3, true);
  }
}
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    if (argc == 4) return fakeWorker(application.arguments()[1]);
    require(argc == 3, "Expected native package and host fixture directories");
    const QDir package(application.arguments()[1]), fixtures(application.arguments()[2]);
    const auto reference = image(fixtures.filePath("reference.rgb"), 256);
    const auto alternate = image(fixtures.filePath("alternate.rgb"), 256);
    const auto content = image(fixtures.filePath("content.rgb"), 384);
    QString error;
    QImage result(3, 3, QImage::Format_RGB32); result.fill(Qt::red);
    const auto sentinel = result;
    NeuralStyle::Session session(package.absolutePath());
    const auto infer = [&](const QImage &style) {
      const bool ok = session.style(style, content, result, error);
      require(ok, qPrintable(error));
    };
    infer(reference);
    compare(result, image(fixtures.filePath("expected.rgb"), 384));
    std::vector<QImage> batch;
    require(session.styleBatch(reference, {content, content}, batch, error), "Batch inference failed");
    require(batch.size()==2 && batch[0]==batch[1], "Held content changed within a batch");
    NeuralStyle::Session second(package.absolutePath());
    require(second.style(reference, content, result, error), "Idle session retained the worker lease");
    auto concurrent=std::async(std::launch::async,[&] {
      NeuralStyle::Session other(package.absolutePath());QImage output;QString diagnostic;
      return other.style(reference,content,output,diagnostic);
    });
    infer(reference);require(concurrent.get(),"Competing session failed to release worker lease");
    infer(alternate);
    compare(result, image(fixtures.filePath("alternate-expected.rgb"), 384));
    infer(reference);
    compare(result, image(fixtures.filePath("expected.rgb"), 384));
    std::atomic_bool cancel{true}; result = sentinel;
    require(!session.style(reference, content, result, error, &cancel) && result == sentinel,
            "Cancellation published a result");
    infer(reference);
    compare(result, image(fixtures.filePath("expected.rgb"), 384));
    auto transparent = reference.convertToFormat(QImage::Format_ARGB32);
    transparent.setPixel(0, 0, 0); result = sentinel;
    require(!session.style(transparent, content, result, error) && result == sentinel,
            "Transparent input silently accepted");
    session.stop();

    QTemporaryDir temporary;
    require(temporary.isValid(), "Temporary component directory unavailable");
    const QDir fake(temporary.path());
    auto manifest = QJsonDocument::fromJson(read(package.filePath("package.json"))).object();
    for (const auto &role : {QString("prediction"), QString("transfer")}) {
      const auto name = manifest.value(role).toObject().value("file").toString();
      require(QFile::copy(package.filePath(name), fake.filePath(name)), "Cannot copy model fixture");
    }
    const QString worker = fake.filePath(QFileInfo(application.applicationFilePath()).fileName());
    require(QFile::copy(application.applicationFilePath(), worker), "Cannot copy protocol fixture worker");
    manifest["worker"] = entry(worker);
    const QString runtime = fake.filePath("fixture-runtime");
    for (const auto &mode : {QByteArray("wrong"), QByteArray("oversized"), QByteArray("crash"), QByteArray("stall"), QByteArray("fragmented")}) {
      write(runtime, mode); manifest["runtime"] = entry(runtime);
      write(fake.filePath("package.json"), QJsonDocument(manifest).toJson());
      NeuralStyle::Session broken(fake.absolutePath());
      result = sentinel; cancel = false;
      std::thread canceller;
      if (mode == "stall") canceller = std::thread([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); cancel = true;
      });
      const bool ok = broken.style(reference, content, result, error, &cancel);
      if (canceller.joinable()) canceller.join();
      if (mode == "fragmented") {
        require(ok, qPrintable(error));
        QImage black(384, 384, QImage::Format_RGB32); black.fill(Qt::black); compare(result, black);
      } else require(!ok && result == sentinel, "Invalid/cancelled worker published pixels");
    }
    write(runtime,"slow-batch");manifest["runtime"]=entry(runtime);
    write(fake.filePath("package.json"),QJsonDocument(manifest).toJson());
    std::vector<QImage> longShot;
    for(int i=0;i<120;++i){auto frame=content.copy();frame.setPixel(0,0,qRgb(i,0,0));longShot.push_back(frame);}
    auto batchOwner=std::async(std::launch::async,[&] {
      NeuralStyle::Session owner(fake.absolutePath());std::vector<QImage> output;QString diagnostic;
      return owner.styleBatch(reference,longShot,output,diagnostic);
    });
    QElapsedTimer ready;ready.start();
    while(!QFileInfo::exists(runtime+".ready") && ready.elapsed()<10000)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(QFileInfo::exists(runtime+".ready"),"Long batch did not acquire the worker");
    auto cancelledWaiter=std::async(std::launch::async,[&] {
      NeuralStyle::Session waiter(fake.absolutePath());QString diagnostic;QImage output=sentinel;
      std::atomic_bool abort{false};std::thread trigger([&]{std::this_thread::sleep_for(std::chrono::milliseconds(200));abort=true;});
      QElapsedTimer elapsed;elapsed.start();
      const bool ok=waiter.style(reference,content,output,diagnostic,&abort);
      trigger.join();return !ok && output==sentinel && elapsed.elapsed()<5000;
    });
    NeuralStyle::Session competitor(fake.absolutePath());QElapsedTimer queued;queued.start();
    require(competitor.style(reference,content,result,error),"Valid request failed while waiting for a long batch");
    require(queued.elapsed()>=30000,"Long contention fixture did not cover the former timeout");
    require(batchOwner.get() && cancelledWaiter.get(),"Long batch or cancelled queue waiter failed");
    auto badEntry = manifest.value("runtime").toObject(); badEntry["bytes"] = 1e300;
    manifest["runtime"] = badEntry;
    write(fake.filePath("package.json"), QJsonDocument(manifest).toJson());
    NeuralStyle::Session invalid(fake.absolutePath()); result = sentinel;
    require(!invalid.style(reference, content, result, error) && result == sentinel,
            "Unbounded component declaration accepted");
    std::cout << "Native Qt host inference and protocol recovery passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
