#include "cadmiumcodec.h"
#include "cadmiumruntime.h"
#include "tregion.h"
#include "trasterimage.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"

#include <QApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
const QColor red(230, 30, 20, 255);
const QColor blue(20, 60, 230, 255);

void require(bool value, const QString &message) {
  if (!value) throw std::runtime_error(message.toStdString());
}

bool linePixel(int x, int y) {
  return x >= 64 && x < 192 && y >= 64 && y < 192 &&
         (x < 68 || x >= 188 || y < 68 || y >= 188);
}

bool interiorPixel(int x, int y) {
  return x >= 68 && x < 188 && y >= 68 && y < 188;
}

TToonzImageP toonzDrawing(bool painted) {
  TRasterCM32P raster(256, 256);
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x)
      raster->pixels(y)[x] = linePixel(x, y) ? TPixelCM32(1, 0, 0)
          : TPixelCM32(0, painted && interiorPixel(x, y) ? 2 : 0, 255);
  TToonzImageP image(raster, raster->getBounds());
  TPaletteP palette = new TPalette;
  palette->addStyle(TPixel32(red.red(), red.green(), red.blue(), red.alpha()));
  image->setPalette(palette.getPointer());
  image->setDpi(120.0, 144.0);
  image->setOffset(TPoint(3, -7));
  return image;
}

TRasterImageP colorDrawing(bool painted) {
  TRaster32P raster(256, 256);
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x)
      raster->pixels(y)[x] = linePixel(x, y) ? TPixel32::Black
          : painted && interiorPixel(x, y)
              ? TPixel32(red.red(), red.green(), red.blue(), red.alpha())
              : TPixel32::Transparent;
  TRasterImageP image(raster);
  image->setDpi(120.0, 144.0);
  image->setOffset(TPoint(3, -7));
  return image;
}

TVectorImageP vectorDrawing(bool painted) {
  TVectorImageP image = new TVectorImage;
  TPaletteP palette = new TPalette;
  palette->addStyle(TPixel32(red.red(), red.green(), red.blue(), red.alpha()));
  image->setPalette(palette.getPointer());
  std::vector<TThickPoint> points = {
      {-100, -100, 2}, {0, -100, 2}, {100, -100, 2},
      {100, 0, 2}, {100, 100, 2}, {0, 100, 2},
      {-100, 100, 2}, {-100, 0, 2}, {-100, -100, 2}};
  TStroke *stroke = new TStroke(points);
  stroke->setSelfLoop(true);
  stroke->setStyle(1);
  image->addStroke(stroke);
  image->findRegions();
  require(image->getRegionCount() == 1, "The vector fixture needs one region.");
  image->getRegion(0U)->setStyle(painted ? 2 : 0);
  return image;
}

QJsonObject requestFor(const Cadmium::Frame &reference,
                       const Cadmium::Frame &target, bool singleColor = true) {
  if (singleColor) {
    require(reference.regionColors.size() == 2 && target.regionColors.size() == 2,
            "The real-model fixtures need exterior and one enclosed region.");
    require(reference.regionColors[1] == red,
            "The adapter did not extract the known reference color.");
  }
  const QString referenceLabels = Cadmium::labelUri(reference);
  const QString targetLabels = Cadmium::labelUri(target);
  const QString referenceLines = Cadmium::lineImageUri(reference.lines);
  const QString targetLines = Cadmium::lineImageUri(target.lines);
  require(!referenceLabels.isEmpty() && !targetLabels.isEmpty() &&
              !referenceLines.isEmpty() && !targetLines.isEmpty(),
          "The native adapters produced invalid transport images.");
  QJsonArray palette;
  for (int region = 0; region < reference.regionColors.size(); ++region) {
    const QColor color = reference.regionColors[region];
    if (!color.isValid())
      palette.append(QJsonArray{-100, 0, 0, 0});
    else
      palette.append(QJsonArray{color.red(), color.green(), color.blue(), color.alpha()});
  }
  const QJsonObject ref{{"seg_map_uri", referenceLabels},
                        {"line_image_uri", referenceLines},
                        {"colors_rgba", palette}};
  return QJsonObject{{"target_seg_map_uri", targetLabels},
                     {"target_line_image_uri", targetLines},
                     {"references", QJsonArray{ref}},
                     {"return_colorized", false}, {"verbose", false}};
}

QJsonObject infer(CadmiumRuntime *runtime, const QJsonObject &request) {
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  bool completed = false;
  QString error;
  QJsonObject result;
  const auto ready = QObject::connect(runtime, &CadmiumRuntime::resultReady,
      &loop, [&](const QJsonObject &response) {
        result = response; completed = true; loop.quit();
      });
  const auto failed = QObject::connect(runtime, &CadmiumRuntime::failed,
      &loop, [&](const QString &message) {
        error = message; completed = true; loop.quit();
      });
  QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
    error = QStringLiteral("The real-model inference timed out.");
    completed = true; loop.quit();
  });
  timeout.start(10 * 60 * 1000);
  runtime->colorize(request);
  if (!completed) loop.exec();
  QObject::disconnect(ready);
  QObject::disconnect(failed);
  require(error.isEmpty(), error);
  require(completed && !result.isEmpty(), "The worker returned no inference result.");
  return result;
}

void prediction(const QJsonObject &response, const Cadmium::Frame &target,
                QVector<int> &styles, QVector<QColor> &colors) {
  require(response.value("target_color_ids").isArray() &&
              response.value("target_colors_rgba").isArray(),
          "The model response omitted region predictions.");
  const QJsonArray ids = response.value("target_color_ids").toArray();
  const QJsonArray rgba = response.value("target_colors_rgba").toArray();
  require(ids.size() == target.regionColors.size() && rgba.size() == ids.size(),
          "The model returned an incorrect region count.");
  styles = QVector<int>(ids.size(), -100);
  colors = QVector<QColor>(ids.size());
  for (int region = 0; region < ids.size(); ++region) {
    require(ids[region].isDouble() && std::isfinite(ids[region].toDouble()) &&
                ids[region].toDouble() == std::floor(ids[region].toDouble()),
            "The model returned a noninteger color identifier.");
    const double id = ids[region].toDouble();
    require(id == -100 || (id >= 0 && id < 2),
            "The model returned a color identifier outside the reference palette.");
    require(rgba[region].isArray() && rgba[region].toArray().size() == 4,
            "The model returned an invalid color tuple.");
    const QJsonArray pixel = rgba[region].toArray();
    for (int channel = 0; channel < 4; ++channel)
      require(pixel[channel].isDouble() && std::isfinite(pixel[channel].toDouble()) &&
                  pixel[channel].toDouble() == std::floor(pixel[channel].toDouble()) &&
                  (pixel[channel].toDouble() == -100 ||
                   (pixel[channel].toDouble() >= 0 && pixel[channel].toDouble() <= 255)),
              "The model returned an invalid color channel.");
    if (!region) continue;
    require(id >= 0, "The model left the known matching region unresolved.");
    const QColor predicted(pixel[0].toInt(), pixel[1].toInt(),
                           pixel[2].toInt(), pixel[3].toInt());
    require(predicted == red, "The model did not propagate the known reference color.");
    styles[region] = 2;
    colors[region] = predicted;
  }
}

void rasterMetadata(const TImageP &original, const TImageP &result) {
  const TToonzImageP cmOriginal(original), cmResult(result);
  const TRasterImageP rgbOriginal(original), rgbResult(result);
  double dx, dy, resultDx, resultDy;
  if (cmOriginal && cmResult) {
    cmOriginal->getDpi(dx, dy); cmResult->getDpi(resultDx, resultDy);
    require(cmOriginal->getOffset() == cmResult->getOffset(), "Native offset changed.");
  } else {
    require(rgbOriginal && rgbResult, "The output raster type changed.");
    rgbOriginal->getDpi(dx, dy); rgbResult->getDpi(resultDx, resultDy);
    require(rgbOriginal->getOffset() == rgbResult->getOffset(), "Raster offset changed.");
  }
  require(dx == resultDx && dy == resultDy, "Native DPI changed.");
}

void toonzPipeline(CadmiumRuntime *runtime) {
  const TToonzImageP reference = toonzDrawing(true), target = toonzDrawing(false);
  Cadmium::Frame ref, frame;
  QString error;
  require(Cadmium::snapshotToonzRaster(TImageP(reference.getPointer()), ref, error), error);
  require(Cadmium::snapshotToonzRaster(TImageP(target.getPointer()), frame, error), error);
  QVector<int> styles;
  QVector<QColor> colors;
  prediction(infer(runtime, requestFor(ref, frame)), frame, styles, colors);
  const TToonzImageP result(Cadmium::applyToonzRaster(frame, styles, colors, false, error));
  require(bool(result), error);
  require(result->getRaster()->pixels(128)[128].getPaint() == 2,
          "Real-model output did not become native editable paint.");
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const TPixelCM32 before = target->getRaster()->pixels(y)[x];
      const TPixelCM32 after = result->getRaster()->pixels(y)[x];
      require(before.getInk() == after.getInk() && before.getTone() == after.getTone(),
              "Inference application changed native ink or tone.");
      require(before.getPaint() == 0, "Inference modified the original target.");
      if (!interiorPixel(x, y))
        require(before == after, "Inference modified protected ink or exterior pixels.");
    }
  rasterMetadata(TImageP(target.getPointer()), TImageP(result.getPointer()));
}

int pairedInterior(int x, int y) {
  if (x >= 32 && x < 96 && y >= 40 && y < 216) return 2;
  const int dx = x - 192, dy = y - 128;
  return dx * dx + dy * dy < 38 * 38 ? 3 : 0;
}

TToonzImageP pairedDrawing(bool painted) {
  TRasterCM32P raster(256, 256);
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const bool rectangle = x >= 28 && x < 100 && y >= 36 && y < 220;
      const int dx = x - 192, dy = y - 128;
      const bool circle = dx * dx + dy * dy < 42 * 42;
      const int paint = pairedInterior(x, y);
      const bool ink = (rectangle || circle) && paint == 0;
      raster->pixels(y)[x] = ink ? TPixelCM32(1, 0, 0)
          : TPixelCM32(0, painted ? paint : 0, 255);
    }
  TToonzImageP image(raster, raster->getBounds());
  TPaletteP palette = new TPalette;
  palette->addStyle(TPixel32(red.red(), red.green(), red.blue(), red.alpha()));
  palette->addStyle(TPixel32(blue.red(), blue.green(), blue.blue(), blue.alpha()));
  image->setPalette(palette.getPointer());
  image->setDpi(120.0, 144.0);
  image->setOffset(TPoint(3, -7));
  return image;
}

void distinctRegionPipeline(CadmiumRuntime *runtime) {
  const TToonzImageP reference = pairedDrawing(true), target = pairedDrawing(false);
  Cadmium::Frame ref, frame;
  QString error;
  require(Cadmium::snapshotToonzRaster(TImageP(reference.getPointer()), ref, error), error);
  require(Cadmium::snapshotToonzRaster(TImageP(target.getPointer()), frame, error), error);
  require(ref.regionColors.size() == 3 && frame.regionColors.size() == 3,
          "The distinct-color fixture needs two enclosed regions and the exterior.");
  const auto labelAt = [](const Cadmium::Frame &drawing, int x, int nativeY) {
    return drawing.labels[(drawing.lines.height() - 1 - nativeY) * drawing.lines.width() + x];
  };
  const int redTarget = labelAt(frame, 64, 128), blueTarget = labelAt(frame, 192, 128);
  const int redReference = labelAt(ref, 64, 128), blueReference = labelAt(ref, 192, 128);
  require(redTarget > 0 && blueTarget > 0 && redTarget != blueTarget &&
              redReference > 0 && blueReference > 0 && redReference != blueReference,
          "Distinct native region positions collapsed into one label.");
  require(ref.regionColors[redReference] == red && ref.regionColors[blueReference] == blue,
          "The reference adapter did not extract distinct native fill colors.");

  const QJsonObject response = infer(runtime, requestFor(ref, frame, false));
  const QJsonArray ids = response.value("target_color_ids").toArray();
  const QJsonArray rgba = response.value("target_colors_rgba").toArray();
  require(ids.size() == 3 && rgba.size() == 3,
          "The distinct-region model response has an incorrect region count.");
  // Match the worker's unique reference palette in first-occurrence order.
  QVector<QColor> palette;
  for (const QColor &color : ref.regionColors)
    if (!palette.contains(color)) palette.append(color);
  QVector<int> styles(3, -100);
  QVector<QColor> colors(3);
  const auto accept = [&](int targetLabel, int referenceLabel, const QColor &expected) {
    const QJsonValue id = ids[targetLabel];
    require(id.isDouble() && id.toDouble() == palette.indexOf(expected),
            "The model did not match the expected distinct reference palette entry.");
    const QJsonArray pixel = rgba[targetLabel].toArray();
    require(pixel.size() == 4, "The model returned an invalid distinct-region color tuple.");
    const int expectedChannels[] = {expected.red(), expected.green(), expected.blue(), expected.alpha()};
    for (int channel = 0; channel < 4; ++channel)
      require(pixel[channel].isDouble() && pixel[channel].toDouble() == expectedChannels[channel],
              "The model assigned the wrong color to a distinct native region.");
    styles[targetLabel] = ref.regionStyles[referenceLabel];
    colors[targetLabel] = expected;
  };
  accept(redTarget, redReference, red);
  accept(blueTarget, blueReference, blue);
  require(styles[redTarget] != styles[blueTarget],
          "Distinct native reference styles were merged.");
  const TToonzImageP result(Cadmium::applyToonzRaster(frame, styles, colors, false, error));
  require(bool(result), error);
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const TPixelCM32 before = target->getRaster()->pixels(y)[x];
      const TPixelCM32 after = result->getRaster()->pixels(y)[x];
      require(before.getPaint() == 0, "Distinct-color inference modified the original target.");
      require(after.getInk() == before.getInk() && after.getTone() == before.getTone(),
              "Distinct-color application changed native ink or tone.");
      require(after.getPaint() == pairedInterior(x, y),
              "Distinct-color inference did not become the correct per-region native paint.");
    }
  rasterMetadata(TImageP(target.getPointer()), TImageP(result.getPointer()));
}

void vectorPipeline(CadmiumRuntime *runtime) {
  const TVectorImageP reference = vectorDrawing(true), target = vectorDrawing(false);
  Cadmium::Frame ref, frame;
  QString error;
  require(Cadmium::snapshotVector(TImageP(reference.getPointer()), ref, error), error);
  require(Cadmium::snapshotVector(TImageP(target.getPointer()), frame, error), error);
  QVector<int> styles;
  QVector<QColor> colors;
  prediction(infer(runtime, requestFor(ref, frame)), frame, styles, colors);
  const TVectorImageP result(Cadmium::applyVector(frame, styles, colors, false, error));
  require(bool(result), error);
  require(target->getRegion(0U)->getStyle() == 0 && result->getRegion(0U)->getStyle() == 2,
          "Real-model vector application failed or modified the source.");
  require(target->getStrokeCount() == result->getStrokeCount(), "Vector strokes changed.");
  for (UINT index = 0; index < target->getStrokeCount(); ++index) {
    const TStroke *before = target->getStroke(index), *after = result->getStroke(index);
    require(before->getId() == after->getId() && before->getStyle() == after->getStyle() &&
                before->isSelfLoop() == after->isSelfLoop() &&
                before->getControlPointCount() == after->getControlPointCount(),
            "Native vector stroke attributes changed.");
    for (int point = 0; point < before->getControlPointCount(); ++point) {
      const TThickPoint a = before->getControlPoint(point), b = after->getControlPoint(point);
      require(a.x == b.x && a.y == b.y && a.thick == b.thick,
              "Real-model application changed vector geometry.");
    }
  }
}

void fullColorPipeline(CadmiumRuntime *runtime) {
  const TRasterImageP reference = colorDrawing(true), target = colorDrawing(false);
  Cadmium::Frame ref, frame;
  ref.rasterSource = Cadmium::RasterSource::LinkedLines;
  ref.lineSource = TImageP(target.getPointer());
  QString error;
  require(Cadmium::snapshotFullColor(TImageP(reference.getPointer()), ref, error), error);
  require(Cadmium::snapshotFullColor(TImageP(target.getPointer()), frame, error), error);
  QVector<int> styles;
  QVector<QColor> colors;
  prediction(infer(runtime, requestFor(ref, frame)), frame, styles, colors);
  const TRasterImageP result(Cadmium::applyFullColor(frame, styles, colors, false, error));
  require(bool(result), error);
  const TRaster32P before = target->getRaster(), after = result->getRaster();
  require(bool(after), "The full-color sample format changed.");
  const TPixel32 expected(red.red(), red.green(), red.blue(), red.alpha());
  require(after->pixels(128)[128] == expected,
          "Real-model output did not become full-color raster paint.");
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      require(before->pixels(y)[x] == (linePixel(x, y) ? TPixel32::Black : TPixel32::Transparent),
              "Inference modified the original full-color target.");
      if (!interiorPixel(x, y))
        require(before->pixels(y)[x] == after->pixels(y)[x],
                "Inference modified protected full-color ink or exterior pixels.");
    }
  rasterMetadata(TImageP(target.getPointer()), TImageP(result.getPointer()));
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cout << "Real-model pipeline requires a worker executable and model directory.\n";
    return 77;
  }
  QApplication application(argc, argv);
  QTemporaryDir settings;
  if (!settings.isValid()) return 1;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
  CadmiumRuntime *runtime = CadmiumRuntime::instance();
  try {
    require(QFileInfo(QString::fromLocal8Bit(argv[1])).isExecutable(),
            "The supplied real-model worker executable is unavailable.");
    require(QFileInfo(QString::fromLocal8Bit(argv[2])).isDir(),
            "The supplied model directory is unavailable.");
    runtime->setStartupMode(CadmiumRuntime::Off);
    runtime->setExecutablePath(QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath());
    runtime->setModelsDirectory(QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath());
    runtime->setStartupMode(CadmiumRuntime::OnDemand);
    toonzPipeline(runtime);
    std::cout << "Real-model Toonz Raster pipeline passed.\n";
    distinctRegionPipeline(runtime);
    std::cout << "Real-model distinct-color region matching passed.\n";
    vectorPipeline(runtime);
    std::cout << "Real-model vector pipeline passed.\n";
    fullColorPipeline(runtime);
    std::cout << "Real-model full-color pipeline passed.\n";
    runtime->stop();
    return 0;
  } catch (const std::exception &error) {
    runtime->stop();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
