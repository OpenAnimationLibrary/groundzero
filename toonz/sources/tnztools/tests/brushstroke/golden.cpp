#include "golden.h"

#include "tregion.h"
#include "trasterimage.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tundo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace brushstroke {

namespace {

// 17 significant digits read back as the same double.
std::string number(double value) {
  return QString::number(value, 'g', 17).toStdString();
}

void recordRegion(Record& record, const std::string& label,
                  const TRegion& region, int depth) {
  const TRectD box = region.getBBox();
  record.lines.push_back(
      label + " region depth=" + std::to_string(depth) +
      " style=" + std::to_string(region.getStyle()) + " subregions=" +
      std::to_string(region.getSubregionCount()) + " box " + number(box.x0) +
      " " + number(box.y0) + " " + number(box.x1) + " " + number(box.y1));
  for (UINT i = 0; i < region.getSubregionCount(); ++i)
    recordRegion(record, label, *region.getSubregion(i), depth + 1);
}

// A pixel as one number that does not depend on the byte order.
quint32 packed(const TPixel32& pixel) {
  return quint32(pixel.m) << 24 | quint32(pixel.r) << 16 |
         quint32(pixel.g) << 8 | quint32(pixel.b);
}

quint32 packed(const TPixelCM32& pixel) {
  return quint32(pixel.getInk()) << 20 | quint32(pixel.getPaint()) << 8 |
         quint32(pixel.getTone());
}

template <class RasterP>
QImage packedImage(const RasterP& raster) {
  QImage image(raster->getLx(), raster->getLy(), QImage::Format_ARGB32);
  for (int y = 0; y < raster->getLy(); ++y) {
    QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
    for (int x = 0; x < raster->getLx(); ++x)
      line[x] = packed(raster->pixels(y)[x]);
  }
  return image;
}

std::string digest(const QImage& image) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  for (int y = 0; y < image.height(); ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const char bytes[4] = {char(line[x] >> 24), char(line[x] >> 16),
                             char(line[x] >> 8), char(line[x])};
      hash.addData(bytes, 4);
    }
  }
  return hash.result().toHex().toStdString();
}

int countPainted(const QImage& image, quint32 empty) {
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x)
      if (line[x] != empty) ++count;
  }
  return count;
}

void recordPacked(Record& record, const std::string& label, const char* kind,
                  const QImage& image, const TRect& savebox, quint32 empty) {
  record.lines.push_back(
      label + " raster kind=" + kind + " size=" +
      std::to_string(image.width()) + "x" + std::to_string(image.height()) +
      " savebox=" + std::to_string(savebox.x0) + "," +
      std::to_string(savebox.y0) + "," + std::to_string(savebox.x1) + "," +
      std::to_string(savebox.y1) + " painted=" +
      std::to_string(countPainted(image, empty)) + " sha256=" + digest(image));
  record.images.push_back(image);
}

QString textPath(const QString& dir, const std::string& name) {
  return dir + "/" + QString::fromStdString(name) + ".txt";
}

QString imagePath(const QString& dir, const std::string& name, int index) {
  return dir + "/" + QString::fromStdString(name) + "." +
         QString::number(index) + ".png";
}

bool isNumber(const QString& token, double& value) {
  bool ok = false;
  value   = token.toDouble(&ok);
  return ok;
}

// "<label> raster ..." or "afterUndo <label> raster ...": a line that has an
// image.
bool isRasterLine(const QString& line) {
  return line.contains(" raster kind=");
}

// How far two images are apart, for the report of a raster that differs.
std::string pixelDifference(const QImage& expected, const QImage& actual) {
  if (expected.size() != actual.size()) return "the sizes differ";
  int different = 0, largest = 0;
  for (int y = 0; y < expected.height(); ++y) {
    const QRgb* a = reinterpret_cast<const QRgb*>(expected.constScanLine(y));
    const QRgb* b = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
    for (int x = 0; x < expected.width(); ++x) {
      if (a[x] == b[x]) continue;
      ++different;
      for (int shift = 0; shift < 32; shift += 8)
        largest = std::max(largest, std::abs(int((a[x] >> shift) & 0xff) -
                                             int((b[x] >> shift) & 0xff)));
    }
  }
  return std::to_string(different) + " pixels differ, by up to " +
         std::to_string(largest) + " in one byte";
}

// Compares two lines that are not equal token by token. Returns what is
// wrong, or nothing when only numbers differ and none by more than the
// tolerance.
std::string compareTokens(const QString& expected, const QString& actual,
                          double tolerance, Comparison& result) {
  const QStringList a = expected.split(' '), b = actual.split(' ');
  if (a.size() != b.size()) return "the fields differ";
  double worst = 0;
  for (int i = 0; i < a.size(); ++i) {
    if (a[i] == b[i]) continue;
    double x, y;
    if (!isNumber(a[i], x) || !isNumber(b[i], y)) return "the fields differ";
    const double difference = std::fabs(x - y);
    ++result.numbersOff;
    result.largest = std::max(result.largest, difference);
    worst          = std::max(worst, difference);
  }
  if (worst <= tolerance) return std::string();
  return "a number differs by " + QString::number(worst, 'g', 3).toStdString();
}

}  // namespace

//-----------------------------------------------------------------------------

void recordVector(Record& record, const std::string& label,
                  const TVectorImageP& image) {
  require(image, label + ": the drawing is not a vector image");
  record.lines.push_back(
      label + " strokes=" + std::to_string(image->getStrokeCount()) +
      " regions=" + std::to_string(image->getRegionCount()) +
      " insideGroup=" + std::to_string(image->isInsideGroup()));
  for (UINT i = 0; i < image->getStrokeCount(); ++i) {
    const TStroke* stroke                  = image->getStroke(i);
    const TStroke::OutlineOptions& options = stroke->outlineOptions();
    record.lines.push_back(
        label + " stroke " + std::to_string(i) +
        " style=" + std::to_string(stroke->getStyle()) +
        " loop=" + std::to_string(stroke->isSelfLoop()) +
        " cap=" + std::to_string(options.m_capStyle) +
        " join=" + std::to_string(options.m_joinStyle) + " miter=" +
        number(options.m_miterLower) + "/" + number(options.m_miterUpper) +
        " group=" + std::to_string(image->getGroupDepth(i)) +
        " points=" + std::to_string(stroke->getControlPointCount()));
    for (int p = 0; p < stroke->getControlPointCount(); ++p) {
      const TThickPoint point = stroke->getControlPoint(p);
      record.lines.push_back(label + " point " + number(point.x) + " " +
                             number(point.y) + " " + number(point.thick));
    }
  }
  for (UINT i = 0; i < image->getRegionCount(); ++i)
    recordRegion(record, label, *image->getRegion(i), 0);
}

void recordRaster(Record& record, const std::string& label,
                  const TImageP& image) {
  if (TToonzImageP toonz = image) {
    recordPacked(record, label, "cm32", packedImage(toonz->getRaster()),
                 toonz->getSavebox(), packed(TPixelCM32()));
    return;
  }
  TRasterImageP raster = image;
  TRaster32P pixels = raster ? TRaster32P(raster->getRaster()) : TRaster32P();
  require(pixels, label + ": the drawing is not a 32 bit raster image");
  recordPacked(record, label, "rgbm32", packedImage(pixels),
               raster->getSavebox(), 0);
}

namespace {
Record levelState(Fixture& fixture) {
  Record state;
  std::vector<TFrameId> fids;
  fixture.level()->getFids(fids);
  for (const TFrameId& fid : fids) {
    const TImageP image     = fixture.level()->getFrame(fid, false);
    const std::string label = "frame" + std::to_string(fid.getNumber());
    if (TVectorImageP vector = image)
      recordVector(state, label, vector);
    else
      recordRaster(state, label, image);
  }
  return state;
}
}  // namespace

Record snapshot(Fixture& fixture) { return levelState(fixture); }

Record changedFrom(Record state, const Record& reference) {
  size_t image = 0;
  for (std::string& line : state.lines) {
    if (!isRasterLine(QString::fromStdString(line))) continue;
    const size_t index = image++;
    if (index >= reference.images.size() ||
        reference.images[index].size() != state.images[index].size())
      continue;
    const QImage &a = reference.images[index], &b = state.images[index];
    int changed = 0;
    for (int y = 0; y < a.height(); ++y) {
      const QRgb* p = reinterpret_cast<const QRgb*>(a.constScanLine(y));
      const QRgb* q = reinterpret_cast<const QRgb*>(b.constScanLine(y));
      for (int x = 0; x < a.width(); ++x)
        if (p[x] != q[x]) ++changed;
    }
    line += " changed=" + std::to_string(changed);
  }
  return state;
}

void append(Record& record, const Record& state, const std::string& prefix) {
  for (const std::string& line : state.lines)
    record.lines.push_back(prefix + line);
  record.images.insert(record.images.end(), state.images.begin(),
                       state.images.end());
}

void recordUndo(Record& record, Fixture& fixture, const Record& before) {
  TUndoManager* undo = TUndoManager::manager();
  const int steps    = undo->getHistoryCount();
  const Record after = levelState(fixture);
  bool undone = true, redone = true;
  for (int i = 0; i < steps; ++i) undone = undo->undo() && undone;
  const Record afterUndo = levelState(fixture);
  for (int i = 0; i < steps; ++i) redone = undo->redo() && redone;
  const Record afterRedo = levelState(fixture);
  record.lines.push_back("undo steps=" + std::to_string(steps) +
                         " undone=" + std::to_string(undone) + " restored=" +
                         std::to_string(afterUndo.lines == before.lines) +
                         " redone=" + std::to_string(redone) + " repeated=" +
                         std::to_string(afterRedo.lines == after.lines));
  if (afterUndo.lines != before.lines)
    append(record, changedFrom(afterUndo, after), "afterUndo ");
  if (afterRedo.lines != after.lines)
    append(record, changedFrom(afterRedo, before), "afterRedo ");
}

void recordChange(Record& record, const Record& before, const Record& after) {
  append(record, changedFrom(after, before), "");
}

//-----------------------------------------------------------------------------

QString platformKey() {
#if defined(_WIN32)
  const QString system = "windows";
#elif defined(__APPLE__)
  const QString system = "macos";
#else
  const QString system = "linux";
#endif
  return system + "-" + QSysInfo::buildCpuArchitecture() + "-qt" +
         QString::number(QT_VERSION_MAJOR);
}

bool goldenExists(const QString& dir, const std::string& name) {
  return QFile::exists(textPath(dir, name));
}

Record loadGolden(const QString& dir, const std::string& name) {
  Record record;
  QFile file(textPath(dir, name));
  require(file.open(QIODevice::ReadOnly | QIODevice::Text),
          "cannot read golden data " + textPath(dir, name).toStdString());
  QTextStream stream(&file);
  int rasters = 0;
  while (!stream.atEnd()) {
    const QString line = stream.readLine();
    if (line.isEmpty()) continue;
    record.lines.push_back(line.toStdString());
    if (!isRasterLine(line)) continue;
    const QString path = imagePath(dir, name, rasters++);
    if (!QFile::exists(path)) continue;
    QImage image(path);
    require(!image.isNull(), "cannot read golden image " + path.toStdString());
    record.images.push_back(image.convertToFormat(QImage::Format_ARGB32));
  }
  if (int(record.images.size()) != rasters) record.images.clear();
  return record;
}

void saveGolden(const QString& dir, const std::string& name,
                const Record& record, bool withImages) {
  require(QDir().mkpath(dir), "cannot create " + dir.toStdString());
  QFile file(textPath(dir, name));
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "cannot write golden data " + textPath(dir, name).toStdString());
  QByteArray text;
  for (const std::string& line : record.lines)
    text += QByteArray::fromStdString(line) + '\n';
  require(file.write(text) == text.size(), "short write of golden data");
  for (int i = 0; withImages && i < int(record.images.size()); ++i)
    require(
        record.images[i].save(imagePath(dir, name, i), "PNG"),
        "cannot write golden image " + imagePath(dir, name, i).toStdString());
}

void removeGolden(const QString& dir, const std::string& name) {
  QFile::remove(textPath(dir, name));
}

Comparison compare(const Record& expected, const Record& actual,
                   double tolerance) {
  Comparison result;
  if (expected.lines.size() != actual.lines.size())
    result.problems.push_back(
        "the result has " + std::to_string(actual.lines.size()) +
        " lines, the golden data " + std::to_string(expected.lines.size()));
  const size_t count = std::min(expected.lines.size(), actual.lines.size());
  size_t image       = 0;
  for (size_t i = 0; i < count; ++i) {
    const QString a   = QString::fromStdString(expected.lines[i]);
    const QString b   = QString::fromStdString(actual.lines[i]);
    const bool raster = isRasterLine(a) && isRasterLine(b);
    if (raster) ++image;
    if (a == b) continue;
    std::string wrong;
    if (!raster)
      wrong = compareTokens(a, b, tolerance, result);
    else if (image <= expected.images.size() && image <= actual.images.size())
      wrong =
          pixelDifference(expected.images[image - 1], actual.images[image - 1]);
    else
      wrong = "the raster differs";
    if (wrong.empty()) continue;
    result.problems.push_back("line " + std::to_string(i + 1) + ": " + wrong +
                              "\n    got  " + actual.lines[i] + "\n    want " +
                              expected.lines[i]);
  }
  return result;
}

}  // namespace brushstroke
