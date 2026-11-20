#pragma once

#include "automation.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class QIODevice;

namespace GroundzeroAutomation {

/// How much of a file intake reads to find its header.
constexpr int IntakeHeaderBytes = 262144;

/// What the first bytes of a file say it is, read without decoding.
///
/// `format` names the container from its signature (png, jpeg, tiff, ...),
/// or "unknown". For PNG the IHDR and the chunks before the first IDAT are
/// read: size, sample depth, colour type, interlace, pHYs density, tRNS and
/// the colour metadata chunks.
struct ImageHeader final {
  QString format = "unknown";
  // Whether the header is a well-formed PNG IHDR with valid field values.
  bool valid = false;
  // Why a PNG header is not valid; empty when it is or the file is not PNG.
  QString problem;
  int width       = 0;
  int height      = 0;
  int bitDepth    = 0;
  int colorType   = -1;
  bool interlaced = false;
  // none, channel (grey+alpha or RGBA) or transparency_chunk (tRNS).
  QString alpha = "none";
  bool dpiKnown = false;
  double dpiX   = 0;
  double dpiY   = 0;
  // gamma, srgb, icc_profile, chromaticities: present and not applied.
  QStringList colorMetadata;
  // grey8, grey_alpha8, rgb8, rgba16, indexed4, rgb5 and so on.
  QString pixel;
  // Why a well-formed header names a variant intake refuses (multi_page,
  // cmyk, planar, photometric); empty otherwise.
  QString unsupported;

  /// The pixel layout when the header is valid, else empty.
  QString pixelFormat() const;
};

/// Read a header from the start of a device, seeking within it for TIFF
/// directories; `name` gives the extension for formats without a signature.
ImageHeader readImageHeader(QIODevice& device, const QString& name);
/// The same over bytes already read.
ImageHeader readImageHeader(const QByteArray& bytes, const QString& name);

/// The extension the contained decoder writes for an intake format, or empty
/// for a format no native reader is admitted for.
QString decoderExtension(const QString& format);

/// Helper side of the contained decode (parse kind image_decode:<ext>).
QJsonObject intakeDecodeSummary(Context& context, const QString& extension);

/// One file name parsed as a native dotted sequence member
/// `<prefix>.<digits>.<extension>`.
struct SequenceName final {
  bool matches = false;
  int number   = 0;
  int digits   = 0;
};

/// Parse `name` against the prefix and extension of a sequence.
SequenceName parseSequenceName(const QString& name, const QString& prefix,
                               const QString& extension);

/// The intake toolset: intake.preflight, intake.import, intake.provenance.
QJsonArray intakeOperations();
QJsonValue executeIntake(Context& context, const QString& method,
                         const QJsonObject& params);

/// vector.export_svg, a core tool beside raster.export_png.
QJsonArray svgExportOperations();
QJsonValue executeSvgExport(Context& context, const QString& method,
                            const QJsonObject& params);

/// The intake records of a document's levels keyed by level name, for the
/// package manifest key `intake`; empty when no level has one.
QJsonObject intakeManifest(Document& document);
/// Restore the records of a reopened package's levels by name.
void adoptIntakeManifest(Document& document, const QJsonObject& manifest);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationIntakeTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
