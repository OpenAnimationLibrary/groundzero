#include "tnzimage.h"
#include "tlevel_io.h"
#include "timage_io.h"
#include "tvectorimage.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tproperty.h"
#include "tenv.h"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "temporary SVG directory");
    const QString stuff = dir.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default"), "isolated settings");
    TEnv::setStuffDir(TFilePath(stuff));
    TEnv::setApplicationFullName("svg_opacity_test");
    initImageIo();
    for (const auto &mode : {L"Centerline", L"Outline"}) {
      TPropertyGroup properties;
      TEnumProperty strokeMode("Stroke Mode");
      strokeMode.addValue(L"Centerline"); strokeMode.addValue(L"Outline"); strokeMode.setValue(mode);
      TEnumProperty quality("Outline Quality"); quality.addValue(L"High");
      properties.bind(strokeMode); properties.bind(quality);
      for (double opacity : {0.0, .327, 1.0}) {
        TPaletteP palette = new TPalette;
        const int style = palette->addStyle(TPixel32(200, 50, 30, 128));
        TVectorImageP image = new TVectorImage; image->setPalette(palette.getPointer());
        auto *stroke = new TStroke(std::vector<TThickPoint>{{0,0,8},{50,0,8},{100,0,8}});
        stroke->setStyle(style); stroke->setOpacity(opacity); image->addStroke(stroke);
        const TFilePath path(dir.filePath("drawing..svg"));
        TFilePath written;
        {
          TLevelWriterP writer(path, &properties);
          TImageWriterP frame = writer->getFrameWriter(TFrameId(1));
          written = frame->getFilePath(); frame->save(TImageP(image));
        }
        QSvgRenderer renderer(written.getQString());
        require(renderer.isValid(), "independent SVG reader rejected export");
        QImage pixels(320, 100, QImage::Format_ARGB32_Premultiplied); pixels.fill(Qt::transparent);
        QPainter painter(&pixels); renderer.render(&painter, QRectF(0,0,320,100)); painter.end();
        int maximum = 0;
        for (int y = 0; y < pixels.height(); ++y)
          for (int x = 0; x < pixels.width(); ++x)
            maximum = std::max(maximum, qAlpha(pixels.pixel(x,y)));
        require(std::abs(maximum - int(std::lround(128 * opacity))) <= 1,
                "SVG did not multiply palette and stroke opacity");
      }
    }
    std::cout << "Native SVG export independently rendered opacity passed\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
