#include "tiio_psd.h"
#include "trasterimage.h"
#include "timageinfo.h"
#include "trop.h"
#include "tpixelutils.h"
#include "tfilepath_io.h"

#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
#if (defined(x64) || defined(__LP64__))
#include "toonz/preferences.h"
#include <QtCore>
#include <QTextCodec>  // not in <QtCore> in Qt 6 (Core5Compat)
#endif

// forward declaration
// class TImageReaderLayerPsd;

// if path = :
//  filename.psd                to load float image. TLevel frames number is 1
//  filename#layerId.psd        to load only one psd layer from layerId. TLevel
//  frames number is 1
//  filename#layerId#frames.psd to load only one psd layer from layerId. TLevel
//  frames number is psd layers number
//  filename#layerId#group.psd  to load only one psd layer from layerId. TLevel
//  frames number is psd layers folder number
//                              where fodler is the folder belongs psd layer.

TLevelReaderPsd::TLevelReaderPsd(const TFilePath &path)
    : TLevelReader(path), m_path(path), m_layerId(0) {
  m_psdreader           = new TPSDReader(m_path);
  TPSDHeaderInfo header = m_psdreader->getPSDHeaderInfo();
  m_lx                  = header.cols;
  m_ly                  = header.rows;
  m_layersCount         = header.layersCount;
  // set m_info
  m_info                   = new TImageInfo();
  m_info->m_lx             = m_lx;
  m_info->m_ly             = m_ly;
  m_info->m_dpix           = header.hres;
  m_info->m_dpiy           = header.vres;
  m_info->m_bitsPerSample  = header.depth;
  m_info->m_samplePerPixel = header.channels;

  QString name     = m_path.getName().c_str();
  QStringList list = name.split("#");
  if (list.size() >= 2) {
    // There's a layer id, otherwise the level is loaded in FLAT mode
    const QString &layerStr = list.at(1);

#ifdef REF_LAYER_BY_NAME
#if (defined(x64) || defined(__LP64__))
    if (layerStr != "frames") {
      QTextCodec *layerNameCodec = QTextCodec::codecForName(
          Preferences::instance()->getLayerNameEncoding().c_str());
      TPSDParser psdparser(m_path);
      m_layerId = psdparser.getLevelIdByName(
          layerNameCodec->fromUnicode(layerStr).toStdString());
    } else {
      m_layerId = layerStr.toInt();
    }
#endif
#else
    m_layerId = layerName.toInt();
#endif
  }
}

//------------------------------------------------

TLevelReaderPsd::~TLevelReaderPsd() { delete m_psdreader; }

//------------------------------------------------

void TLevelReaderPsd::load(TRasterImageP &rasP, int shrinkX, int shrinkY,
                           TRect region) {
  QMutexLocker sl(&m_mutex);
  TRasterImageP img;
  m_psdreader->setShrink(shrinkX, shrinkY);
  m_psdreader->setRegion(region);
  m_psdreader->load(img, m_layerId);  // if layerId==0 load Merged
  rasP = img;
}

TLevelP TLevelReaderPsd::loadInfo() {
  TPSDParser *psdparser = new TPSDParser(m_path);
  assert(m_layerId >= 0);
  int framesCount = psdparser->getFramesCount(m_layerId);
  TLevelP level;
  level->setName(psdparser->getLevelName(m_layerId));
  m_frameTable.clear();
  for (int i = 0; i < framesCount; i++) {
    TFrameId frame(i + 1);
    m_frameTable.insert(
        std::make_pair(frame, psdparser->getFrameId(m_layerId, i)));
    level->setFrame(frame, TImageP());
  }
  return level;
}

//------------------------------------------------

TLevelReader *TLevelReaderPsd::create(const TFilePath &f) {
  TLevelReaderPsd *reader = new TLevelReaderPsd(f);
  return reader;
}

//-----------------------------------------------------------
//  TImageReaderLayerPsd
//-----------------------------------------------------------

class TImageReaderLayerPsd final : public TImageReader {
public:
  TImageReaderLayerPsd(const TFilePath &, int layerId, TLevelReaderPsd *lr,
                       TImageInfo *info);
  ~TImageReaderLayerPsd() { m_lr->release(); }

private:
  // not implemented
  TImageReaderLayerPsd(const TImageReaderLayerPsd &);
  TImageReaderLayerPsd &operator=(const TImageReaderLayerPsd &src);

public:
  TImageP load() override;

  const TImageInfo *getImageInfo() const override { return m_info; }

private:
  TPSDReader *m_psdreader;
  TLevelReaderPsd *m_lr;
  TImageInfo *m_info;
  int m_layerId;
};

TImageReaderLayerPsd::TImageReaderLayerPsd(const TFilePath &path, int layerId,
                                           TLevelReaderPsd *lr,
                                           TImageInfo *info)
    : TImageReader(path), m_lr(lr), m_layerId(layerId), m_info(info) {
  m_lr->setLayerId(layerId);
  m_lr->addRef();
}

TImageP TImageReaderLayerPsd::load() {
  TRasterImageP rasP;
  m_lr->load(rasP, m_shrink, m_shrink, m_region);
  return TRasterImageP(rasP);
}

TImageReaderP TLevelReaderPsd::getFrameReader(TFrameId fid) {
  int layerId = m_frameTable[fid];
  TImageReaderLayerPsd *ir =
      new TImageReaderLayerPsd(m_path, layerId, this, m_info);
  return TImageReaderP(ir);
}

//------------------------------------------------------------------------------
//  TImageWriterPsd
//------------------------------------------------------------------------------

class TImageWriterPsd final : public TImageWriter {
public:
  TImageWriterPsd(const TFilePath &, TLevelWriterPsd *);
  ~TImageWriterPsd() { m_lwm->release(); }

private:
  // not implemented
  TImageWriterPsd(const TImageWriterPsd &);
  TImageWriterPsd &operator=(const TImageWriterPsd &src);

public:
  void save(const TImageP &) override;

private:
  TLevelWriterPsd *m_lwm;
};

//-----------------------------------------------------------
//-----------------------------------------------------------
//        TImageWriterPsd
//-----------------------------------------------------------

TImageWriterPsd::TImageWriterPsd(const TFilePath &path, TLevelWriterPsd *lwm)
    : TImageWriter(path), m_lwm(lwm) {
  m_lwm->addRef();
}

//-----------------------------------------------------------
void TImageWriterPsd::save(const TImageP &img) { m_lwm->save(img, m_path); }

//-----------------------------------------------------------
//        PSD writing
//-----------------------------------------------------------

namespace {

// PSD (version 1) stores every multi-byte value big-endian.
class PsdOutput {
  FILE *m_file;

public:
  explicit PsdOutput(FILE *file) : m_file(file) {}

  void put8(unsigned int value) { fputc((int)(value & 0xff), m_file); }
  void put16(unsigned int value) {
    put8(value >> 8);
    put8(value);
  }
  void put32(std::uint32_t value) {
    put16(value >> 16);
    put16(value & 0xffff);
  }
  void putZeros(std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) put8(0);
  }
  void putBytes(const char *data, size_t count) {
    if (count) fwrite(data, 1, count, m_file);
  }
  void putBytes(const std::vector<unsigned char> &data) {
    if (!data.empty()) fwrite(data.data(), 1, data.size(), m_file);
  }
};

// The data of one channel that follows its 2-byte compression field: the
// byte count of each row (RLE only), then the rows top to bottom.
struct PsdChannelData {
  std::vector<unsigned char> rowCounts;
  std::vector<unsigned char> rows;

  std::uint64_t size() const { return rowCounts.size() + rows.size(); }
};

inline std::uint64_t padTo4(std::uint64_t size) { return (size + 3) & ~3ULL; }

// 16.16 fixed point, as used by the ResolutionInfo resource.
std::uint32_t toFixed(double value) {
  return (std::uint32_t)(value * 65536.0 + 0.5);
}

// Appends the PackBits encoding (PSD compression 1) of src[0, count) to out.
void packBits(const unsigned char *src, int count,
              std::vector<unsigned char> &out) {
  int i = 0;
  while (i < count) {
    int run = 1;
    while (i + run < count && run < 128 && src[i + run] == src[i]) ++run;
    if (run > 1) {
      // Repeat packet: header 1 - run as a signed byte, then the byte.
      out.push_back((unsigned char)(257 - run));
      out.push_back(src[i]);
      i += run;
      continue;
    }
    // Literal packet of up to 128 bytes, ending before a run of 3 or more.
    int start = i;
    while (i < count && i - start < 128) {
      if (i + 2 < count && src[i] == src[i + 1] && src[i] == src[i + 2]) break;
      ++i;
    }
    out.push_back((unsigned char)(i - start - 1));
    out.insert(out.end(), src + start, src + i);
  }
}

}  // namespace

//-----------------------------------------------------------

void TLevelWriterPsd::save(const TImageP &img, const TFilePath &path) {
  TRasterImageP ri = img;
  if (!ri)
    throw TImageException(path, "Only raster images can be saved as PSD");

  TRasterP ras     = ri->getRaster();
  TRaster32P ras32 = ras;
  TRaster64P ras64 = ras;
  if (!ras32 && !ras64) {
    // Convert the other raster types like TImageWriter::save() does
    if (TRasterFP rasF = ras) {
      ras64 = TRaster64P(ras->getSize());
      TRop::convert(ras64, rasF);
    } else if (TRasterGR8P rasGr = ras) {
      ras32 = TRaster32P(ras->getSize());
      TRop::convert(ras32, rasGr);
    } else
      throw TImageException(path, "unsupported raster type");
  }
  if (ras32)
    ras = ras32;
  else
    ras = ras64;

  const int lx = ras->getLx();
  const int ly = ras->getLy();
  if (lx <= 0 || ly <= 0 || lx > 30000 || ly > 30000)
    throw TImageException(path, "Image size not supported by PSD files");

  // 8 bit channels are RLE (PackBits) compressed, 16 bit ones are stored raw
  const bool is16Bit     = !ras32;
  const int depth        = is16Bit ? 16 : 8;
  const int compression  = is16Bit ? 0 : 1;
  const int rowBytes     = is16Bit ? 2 * lx : lx;
  const int channelCount = 4;

  // Channel planes in R, G, B, A order
  PsdChannelData channels[channelCount];
  std::vector<unsigned char> rowData[channelCount];
  for (int c = 0; c < channelCount; ++c) rowData[c].resize(rowBytes);

  ras->lock();
  for (int y = 0; y < ly; ++y) {
    // Toonz rasters are bottom-up, PSD rows are top-down
    const int rasY = ly - 1 - y;
    // PSD pixels are not premultiplied
    if (ras32) {
      const TPixel32 *pix = ras32->pixels(rasY);
      for (int x = 0; x < lx; ++x) {
        TPixel32 p = pix[x];
        if (p.m != 0) depremult(p);
        rowData[0][x] = p.r;
        rowData[1][x] = p.g;
        rowData[2][x] = p.b;
        rowData[3][x] = p.m;
      }
    } else {
      const TPixel64 *pix = ras64->pixels(rasY);
      for (int x = 0; x < lx; ++x) {
        TPixel64 p = pix[x];
        if (p.m != 0) depremult(p);
        const unsigned int values[channelCount] = {p.r, p.g, p.b, p.m};
        for (int c = 0; c < channelCount; ++c) {
          rowData[c][2 * x]     = (unsigned char)(values[c] >> 8);
          rowData[c][2 * x + 1] = (unsigned char)(values[c] & 0xff);
        }
      }
    }

    for (int c = 0; c < channelCount; ++c) {
      PsdChannelData &channel = channels[c];
      if (compression == 1) {
        const size_t start = channel.rows.size();
        packBits(rowData[c].data(), rowBytes, channel.rows);
        const size_t count = channel.rows.size() - start;
        channel.rowCounts.push_back((unsigned char)(count >> 8));
        channel.rowCounts.push_back((unsigned char)(count & 0xff));
      } else
        channel.rows.insert(channel.rows.end(), rowData[c].begin(),
                            rowData[c].end());
    }
  }
  ras->unlock();

  double dpix = 0.0, dpiy = 0.0;
  ri->getDpi(dpix, dpiy);
  if (!(dpix > 0.0)) dpix = 72.0;
  if (!(dpiy > 0.0)) dpiy = dpix;
  dpix = std::min(dpix, 30000.0);
  dpiy = std::min(dpiy, 30000.0);

  // The layer is named after the frame file, e.g. "name.0001". The legacy
  // Pascal name is kept ASCII; the 'luni' block holds the real name.
  QString layerName = QFileInfo(QString::fromStdWString(path.getWideString()))
                          .completeBaseName()
                          .left(255);
  if (layerName.isEmpty()) layerName = "Layer 1";
  std::string pascalName;
  for (const QChar &ch : layerName) {
    const ushort code = ch.unicode();
    pascalName.push_back(code >= 0x20 && code < 0x7f ? (char)code : '_');
  }
  const std::uint64_t pascalSize = padTo4(1 + pascalName.size());
  const std::uint64_t unicodeSize =
      padTo4(4 + 2 * (std::uint64_t)layerName.size());

  // PSD channel ids of the layer record, and the matching plane
  const int layerChannelIds[channelCount]    = {-1, 0, 1, 2};
  const int layerChannelPlanes[channelCount] = {3, 0, 1, 2};

  // Layer mask data, blending ranges, name, 'luni' additional info
  const std::uint64_t extraSize = 4 + (4 + 40) + pascalSize + 12 + unicodeSize;
  std::uint64_t layerInfoContent =
      2 + 16 + 2 + 6 * channelCount + 16 + extraSize;
  for (int c = 0; c < channelCount; ++c)
    layerInfoContent += 2 + channels[c].size();
  const std::uint64_t layerInfoSize = padTo4(layerInfoContent);
  const std::uint64_t layerMaskSize = 4 + layerInfoSize + 4;
  if (layerMaskSize > 0x7fffffffULL)
    throw TImageException(path, "Image too large for PSD files");

  FILE *file = fopen(path, "wb");
  if (!file) throw TImageException(path, "Can't write file");
  PsdOutput out(file);

  // File header
  out.putBytes("8BPS", 4);
  out.put16(1);  // version
  out.putZeros(6);
  out.put16(channelCount);
  out.put32(ly);
  out.put32(lx);
  out.put16(depth);
  out.put16(ModeRGBColor);

  // Color mode data
  out.put32(0);

  // Image resources: ResolutionInfo (1005), in pixels per inch
  out.put32(4 + 2 + 2 + 4 + 16);
  out.putBytes("8BIM", 4);
  out.put16(1005);
  out.put16(0);  // empty name, padded to an even size
  out.put32(16);
  out.put32(toFixed(dpix));
  out.put16(1);  // pixels per inch
  out.put16(1);  // width unit: inches
  out.put32(toFixed(dpiy));
  out.put16(1);  // pixels per inch
  out.put16(1);  // height unit: inches

  // Layer and mask information
  out.put32((std::uint32_t)layerMaskSize);
  out.put32((std::uint32_t)layerInfoSize);
  // -1: one layer, and the merged image data has an alpha channel
  out.put16(0xffff);

  // Layer record
  out.put32(0);   // top
  out.put32(0);   // left
  out.put32(ly);  // bottom
  out.put32(lx);  // right
  out.put16(channelCount);
  for (int i = 0; i < channelCount; ++i) {
    out.put16(layerChannelIds[i] & 0xffff);
    out.put32((std::uint32_t)(2 + channels[layerChannelPlanes[i]].size()));
  }
  out.putBytes("8BIMnorm", 8);
  out.put8(255);   // opacity
  out.put8(0);     // clipping: base
  out.put8(0x08);  // visible, pixel data relevant
  out.put8(0);     // filler
  out.put32((std::uint32_t)extraSize);
  out.put32(0);  // no layer mask
  // Blending ranges: gray and the 4 channels, source and destination, all
  // with the full 0-255 range
  out.put32(40);
  for (int i = 0; i < 10; ++i) {
    out.put16(0x0000);
    out.put16(0xffff);
  }
  out.put8((unsigned int)pascalName.size());
  out.putBytes(pascalName.data(), pascalName.size());
  out.putZeros(pascalSize - 1 - pascalName.size());
  out.putBytes("8BIMluni", 8);
  out.put32((std::uint32_t)unicodeSize);
  out.put32(layerName.size());
  for (const QChar &ch : layerName) out.put16(ch.unicode());
  out.putZeros(unicodeSize - 4 - 2 * (std::uint64_t)layerName.size());

  // Layer channel image data, in the order of the layer record
  for (int i = 0; i < channelCount; ++i) {
    const PsdChannelData &channel = channels[layerChannelPlanes[i]];
    out.put16(compression);
    out.putBytes(channel.rowCounts);
    out.putBytes(channel.rows);
  }
  out.putZeros(layerInfoSize - layerInfoContent);

  // Global layer mask info
  out.put32(0);

  // Merged image data: one compression field, then all the row counts, then
  // all the rows
  out.put16(compression);
  for (int c = 0; c < channelCount; ++c) out.putBytes(channels[c].rowCounts);
  for (int c = 0; c < channelCount; ++c) out.putBytes(channels[c].rows);

  bool failed = ferror(file) != 0;
  if (fclose(file) != 0) failed = true;
  if (failed) throw TImageException(path, "Can't write file");
}

//-----------------------------------------------------------
//        TLevelWriterPsd
//-----------------------------------------------------------

TLevelWriterPsd::TLevelWriterPsd(const TFilePath &path, TPropertyGroup *winfo)
    : TLevelWriter(path, winfo) {}

//-----------------------------------------------------------

TLevelWriterPsd::~TLevelWriterPsd() {}

//-----------------------------------------------------------

// Each frame is written to its own file, e.g. name..psd -> name.0001.psd.
// TFilePath::withFrame() is not used since psd is a level type, which it
// numbers inconsistently across the file path rules.
TFilePath TLevelWriterPsd::getFramePath(TFrameId fid) const {
  if (fid.isNoFrame() || fid.isEmptyFrame()) return m_path;

  // change the frame format with the template, as TLevelWriter does
  if (!m_frameFormatTemplateFId.isNoFrame()) {
    fid.setZeroPadding(m_frameFormatTemplateFId.getZeroPadding());
    fid.setStartSeqInd(m_frameFormatTemplateFId.getStartSeqInd());
  }

  QString name = QFileInfo(QString::fromStdWString(
                               m_path.withoutParentDir().getWideString()))
                     .completeBaseName();
  if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('_')))
    name.chop(1);
  if (fid.getStartSeqInd() != '\0') name += QLatin1Char(fid.getStartSeqInd());
  name += QString::fromStdString(fid.expand(fid.getCurrentFormat()));
  name += QString::fromStdString(m_path.getDottedType());
  return m_path.getParentDir() + TFilePath(name.toStdWString());
}

//-----------------------------------------------------------

TImageWriterP TLevelWriterPsd::getFrameWriter(TFrameId fid) {
  TImageWriterPsd *iwm = new TImageWriterPsd(getFramePath(fid), this);
  return TImageWriterP(iwm);
}
