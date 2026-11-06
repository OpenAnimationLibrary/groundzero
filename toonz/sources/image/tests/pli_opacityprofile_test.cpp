#include "tnzimage.h"
#include "tlevel_io.h"
#include "tvectorimage.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tthread.h"
#include "texception.h"
#include "tenv.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
QByteArray read(const QString &path) {
  QFile file(path); require(file.open(QIODevice::ReadOnly), "cannot open PLI fixture");
  return file.readAll();
}
void write(const QString &path, const QByteArray &bytes) {
  QFile file(path); require(file.open(QIODevice::WriteOnly), "cannot create corrupted PLI fixture");
  require(file.write(bytes) == bytes.size(), "short corrupted fixture write");
}
void append32(QByteArray &bytes, std::uint32_t value) {
  const quint32 little = qToLittleEndian(quint32(value));
  bytes.append(reinterpret_cast<const char *>(&little), sizeof(little));
}
QByteArray binary64(double value) {
  std::uint64_t bits; std::memcpy(&bits, &value, sizeof(bits));
  QByteArray bytes; append32(bytes, std::uint32_t(bits >> 32)); append32(bytes, std::uint32_t(bits));
  return bytes;
}
TVectorImageP load(const QString &path, int frame = 1) {
  TLevelReaderP reader{TFilePath(path)};
  reader->loadInfo();
  return reader->getFrameReader(TFrameId(frame))->load();
}
void rejects(const QString &path, const QByteArray &bytes) {
  write(path, bytes);
  bool rejected = false;
  try { rejected = !load(path); }
  catch (const TException &) { rejected = true; }
  catch (const std::exception &) { rejected = true; }
  require(rejected, "malformed/version-incompatible opacity profile PLI accepted");
}
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir dir; require(dir.isValid(), "temporary PLI directory unavailable");
    const QString stuff = dir.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default"), "isolated PLI settings");
    TEnv::setStuffDir(TFilePath(stuff));
    TEnv::setApplicationFullName("pli_opacityprofile_test");
    TThread::init(); initImageIo();
    TPaletteP palette = new TPalette;
    TVectorImageP image = new TVectorImage;
    image->setPalette(palette.getPointer());
    auto *stroke = new TStroke(std::vector<TThickPoint>{{0,0,2},{5,10,2},{10,0,2}});
    stroke->setStyle(1); stroke->setOpacity(.327); image->addStroke(stroke);
    const auto ordinary = dir.filePath("ordinary.pli");
    {
      TLevelWriterP writer{TFilePath(ordinary)};
      writer->getFrameWriter(TFrameId(1))->save(image);
    }
    const auto ordinaryBytes = read(ordinary);
    require(ordinaryBytes.size() > 6 && static_cast<unsigned char>(ordinaryBytes[5]) == 0,
            "ordinary stroke was upgraded to a new PLI version");
    require(load(ordinary)->getStroke(0)->getOpacityProfile().empty(), "legacy drawing gained an envelope");
    const StrokeOpacity::Profile profile{{0.,.2}, {0.,.7}, {.12345678901234567,.43210987654321},
        {.5,.8}, {.5,.3}, {.5000000000000001,.4}, {1.,.9}};
    require(stroke->setOpacityProfile(profile), "valid profile fixture rejected");
    const auto path = dir.filePath("profile.pli");
    {
      TLevelWriterP writer{TFilePath(path)};
      writer->getFrameWriter(TFrameId(1))->save(image);
      TVectorImageP ordinaryFrame = image->clone(); ordinaryFrame->getStroke(0)->setOpacityProfile({});
      writer->getFrameWriter(TFrameId(2))->save(ordinaryFrame);
    }
    const auto bytes = read(path);
    require(bytes.size() > 6 && static_cast<unsigned char>(bytes[4]) == 150 &&
                static_cast<unsigned char>(bytes[5]) == 1,
            "profile PLI did not retain required 150.1 version across ordinary frames");
    TVectorImageP loaded = load(path);
    const auto &actual = loaded->getStroke(0)->getOpacityProfile();
    require(actual.size() == profile.size(), "profile knot count lost on native PLI reload");
    for (size_t i = 0; i < profile.size(); ++i)
      require(actual[i] == profile[i], "PLI quantized a nearby knot or discontinuity");
    require(loaded->getStroke(0)->getOpacity() == .327, "PLI lost global stroke opacity");
    require(load(path, 2)->getStroke(0)->getOpacityProfile().empty(), "envelope leaked into ordinary following frame");

    QByteArray payload; append32(payload, std::uint32_t(profile.size()));
    for (const auto &p : profile) { payload += binary64(p.x); payload += binary64(p.y); }
    const int position = bytes.indexOf(payload);
    require(position >= 0 && bytes.indexOf(payload, position + 1) < 0, "native profile payload not uniquely found");
    const auto corruptPath = dir.filePath("corrupt.pli");
    auto corrupt = bytes;
    QByteArray excessive; append32(excessive, std::uint32_t(StrokeOpacity::MaxKnots + 1));
    corrupt.replace(position, 4, excessive); rejects(corruptPath, corrupt);
    corrupt = bytes; corrupt.replace(position + 4 + 16, 8, binary64(-.1)); rejects(corruptPath, corrupt);
    corrupt = bytes; corrupt.replace(position + 4 + 16 + 8, 8,
        binary64(std::numeric_limits<double>::quiet_NaN())); rejects(corruptPath, corrupt);
    corrupt = bytes.left(position + payload.size() - 1); rejects(corruptPath, corrupt);
    corrupt = bytes; corrupt[5] = char(2); rejects(corruptPath, corrupt);
    std::cout << "Native PLI opacity profile persistence and corruption checks passed\n";
    return 0;
  } catch (const TException &e) {
    std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  return 1;
}
