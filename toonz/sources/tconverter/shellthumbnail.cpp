#include "shellthumbnail.h"
#include "toonz/shellthumbnail.h"
#include "toonzqt/icongenerator.h"
#include "tenv.h"
#include "tiio_std.h"
#include "tnzimage.h"
#include "tsystem.h"
#include "tthread.h"
#include "tfilepath.h"
#include "tpixel.h"

#include <QGuiApplication>
#include <QFileInfo>
#include <QDir>
#include <QImage>
#include <QSaveFile>
#include <QStringList>
#include <algorithm>
#include <iostream>

// These are the same exported initialization entry points used by tcomposer.
DV_IMPORT_API void initStdFx();
DV_IMPORT_API void initColorFx();

int runShellThumbnail(int argc, char **argv) {
  // The Shell host never constructs a GUI application. Only this disposable
  // child owns the platform plugin, render context and image/style registries.
  QGuiApplication application(argc, argv);
  QCoreApplication::setOrganizationName("OpenToonz");
  QCoreApplication::setApplicationName("ToonzPreview");
  const auto args = application.arguments();
  if (args.size() != 5 || args.at(1) != QLatin1String(ShellThumbnail::Command)) return 2;
  bool validSize = false;
  const unsigned size = args.at(3).toUInt(&validSize);
  const QFileInfo input(args.at(2)), output(args.at(4));
  if (!validSize || size == 0 || size > ShellThumbnail::MaxSide ||
      !input.isAbsolute() || !input.isFile() || !output.isAbsolute() ||
      output.exists() || !output.dir().exists()) return 2;
  const QString type = input.suffix().toLower();
  if (type != "pli" && type != "tnz" && type != "tlv" && type != "tga") return 2;
  try {
    TThread::init();
    TEnv::setRootVarName("TOONZROOT"); TEnv::setSystemVarPrefix("TOONZ");
    TEnv::setApplicationFileName(QCoreApplication::applicationFilePath().toStdString());
    TEnv::initUserStuffDir();
    TSystem::hasMainLoop(false);
    Tiio::defineStd(); initImageIo(); initStdFx(); initColorFx();
    // IconGenerator's ten-pixel fitting margin requires a larger source for
    // very small Shell requests. Reduce afterward without exceeding cx.
    const int side = std::max(16, int(size));
    const TDimension requested(side, side);
    IconGenerator::setFilmstripIconSize(requested);
    const TFilePath path(input.absoluteFilePath());
    TRaster32P raster;
    if (type == "pli")
      raster = IconGenerator::generateVectorFileIcon(path, requested, TFrameId::NO_FRAME);
    else if (type == "tnz")
      raster = IconGenerator::generateSceneFileIcon(path, requested, -5);
    else
      raster = IconGenerator::generateRasterFileIcon(path, requested,
          type == "tga" ? path.analyzePath().fId : TFrameId::NO_FRAME);
    if (!raster || raster->getLx() <= 0 || raster->getLy() <= 0 ||
        raster->getLx() > int(ShellThumbnail::MaxSide) ||
        raster->getLy() > int(ShellThumbnail::MaxSide)) return 3;
    QImage pixels(raster->getLx(), raster->getLy(), QImage::Format_ARGB32_Premultiplied);
    if (pixels.isNull()) return 3;
    raster->lock();
    for (int y = 0; y < raster->getLy(); ++y) {
      auto *target = reinterpret_cast<QRgb *>(pixels.scanLine(raster->getLy() - 1 - y));
      const auto *source = raster->pixels(y);
      for (int x = 0; x < raster->getLx(); ++x)
        target[x] = qRgba(source[x].r, source[x].g, source[x].b, source[x].m);
    }
    raster->unlock();
    if (pixels.width() > int(size) || pixels.height() > int(size))
      pixels = pixels.scaled(int(size), int(size), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (pixels.isNull()) return 3;
    QSaveFile file(output.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly)) return 4;
    const auto header = ShellThumbnail::header(pixels.width(), pixels.height());
    if (file.write(reinterpret_cast<const char *>(header.data()), header.size()) != header.size()) return 4;
    QByteArray row(pixels.width() * 4, '\0');
    for (int y = pixels.height() - 1; y >= 0; --y) {
      const auto *source = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
      for (int x = 0; x < pixels.width(); ++x) {
        row[x * 4] = char(qBlue(source[x])); row[x * 4 + 1] = char(qGreen(source[x]));
        row[x * 4 + 2] = char(qRed(source[x])); row[x * 4 + 3] = char(qAlpha(source[x]));
      }
      if (file.write(row) != row.size()) return 4;
    }
    return file.commit() ? 0 : 4;
  } catch (...) {
    // No dialog or overwrite prompt can block an Explorer request. The parent
    // also enforces a wall-clock deadline for decoder/driver failures.
    std::cerr << "Shell thumbnail rendering failed" << std::endl;
    return 3;
  }
}
