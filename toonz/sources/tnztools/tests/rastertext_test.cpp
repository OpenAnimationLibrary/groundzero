// The Type tool's raster characters: TFont::drawText must return an image that
// holds all the ink of a glyph, with glyphOrigin locating it from the pen.
// The ink of italics and of tails like the one of "j" can go left of the pen
// or past the advance.
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

// tfont.h uses QImage without including it
#include "tfont.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

QImage grayText(const QFont &font, const QString &text, int width, int height,
                int penX) {
  QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::white);
  QPainter painter(&image);
  painter.setFont(font);
  painter.setPen(Qt::black);
  painter.drawText(penX, QFontMetrics(font).ascent(), text);
  painter.end();
  return image.convertToFormat(QImage::Format_Grayscale8);
}

// Returns whether the glyph has ink left of the pen
bool checkGlyph(TFontManager *manager, const QFont &font, const QString &text,
                const std::string &name) {
  QImage drawn;
  TPoint origin;
  manager->drawText(drawn, origin, text);
  if (drawn.isNull()) return false;

  // The same text with a margin wide enough for any ink, at the same height
  const QFontMetrics metrics(font);
  const int margin = 2 * metrics.height() + 8;
  const QImage reference =
      grayText(font, text, metrics.horizontalAdvance(text) + 2 * margin,
               drawn.height(), margin);
  bool inkLeftOfPen = false;
  for (int y = 0; y < reference.height(); ++y) {
    const uchar *row = reference.constScanLine(y);
    for (int x = 0; x < reference.width(); ++x) {
      if (row[x] == 255) continue;
      if (x < margin) inkLeftOfPen = true;
      const int dx = x - margin - origin.x;
      // The rasterizer can round an edge pixel differently on another canvas
      if (dx < 0 || dx >= drawn.width() ||
          std::abs(drawn.constScanLine(y)[dx] - row[x]) > 8) {
        std::cerr << "FAIL: " << name << ": the ink at " << x - margin << ","
                  << y << " from the pen is missing (image " << drawn.width()
                  << " wide from " << origin.x << ")" << std::endl;
        ++failures;
        return inkLeftOfPen;
      }
    }
  }
  return inkLeftOfPen;
}
}  // namespace

int main(int argc, char *argv[]) {
  QGuiApplication app(argc, argv);
  TFontManager *manager = TFontManager::instance();
  try {
    manager->loadFontNames();
  } catch (...) {
    std::cerr << "FAIL: no fonts" << std::endl;
    return 1;
  }

  std::vector<std::wstring> families;
  manager->getAllFamilies(families);
  const int size     = 64;
  int checked        = 0;
  int inkLeftOfPen   = 0;
  const QString text = "jfJygpQ/(";
  for (const std::wstring &family : families) {
    try {
      manager->setFamily(family);
    } catch (...) {
      continue;
    }
    std::vector<std::wstring> typefaces;
    manager->getAllTypefaces(typefaces);
    for (const std::wstring &typeface : typefaces) {
      try {
        manager->setTypeface(typeface);
        manager->setSize(size);
      } catch (...) {
        continue;
      }
      const QString qFamily = QString::fromStdWString(family);
      const QString qStyle  = QString::fromStdWString(typeface);
      QFont font(qFamily, size);
      font.setBold(manager->isBold(qFamily, qStyle));
      font.setItalic(manager->isItalic(qFamily, qStyle));
      for (const QChar &c : text) {
        const std::string name =
            (qFamily + " " + qStyle + " '" + c + "'").toStdString();
        if (checkGlyph(manager, font, QString(c), name)) ++inkLeftOfPen;
        ++checked;
      }
      if (checked >= 2000) break;
    }
    if (checked >= 2000) break;
  }

  std::cout << checked << " glyphs checked, " << inkLeftOfPen
            << " with ink left of the pen" << std::endl;
  if (!checked) {
    std::cerr << "FAIL: no font could be set" << std::endl;
    return 1;
  }
  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "rastertext_test passed" << std::endl;
  return 0;
}
