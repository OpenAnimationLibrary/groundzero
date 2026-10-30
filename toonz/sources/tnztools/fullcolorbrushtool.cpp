

#include "fullcolorbrushtool.h"
#include "toonz/rasterbrushimport.h"

// TnzTools includes
#include "tools/tool.h"
#include "tools/cursors.h"
#include "tools/toolutils.h"
#include "tools/toolhandle.h"
#include "tools/tooloptions.h"
#include "tools/replicator.h"

#include "mypainttoonzbrush.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/brushpresetbridge.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/styleselection.h"
#include "tundo.h"

// TnzLib includes
#include "toonz/tpalettehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tframehandle.h"
#include "toonz/ttileset.h"
#include "toonz/ttilesaver.h"
#include "toonz/strokegenerator.h"
#include "toonz/tstageobject.h"
#include "toonz/palettecontroller.h"
#include "toonz/mypaintbrushstyle.h"
#include "toonz/imagestyles.h"
#include "toonz/preferences.h"
#include "toonz/toonzfolders.h"
#include "toonz/txshsimplelevel.h"

// TnzCore includes
#include "tgl.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tenv.h"
#include "tpalette.h"
#include "trop.h"
#include "tstream.h"
#include "tstroke.h"
#include "timagecache.h"
#include "tpixelutils.h"
#include "tsystem.h"
#include "tcolorstyles.h"

// Qt includes
#include <QCoreApplication>  // Qt translation support
#include <algorithm>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QPixmap>
#include <QPainter>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QColorDialog>
#include <QProgressBar>
#include <QThread>
#include <QListWidget>
#include <QSet>
#include <cmath>

//----------------------------------------------------------------------------------

TEnv::IntVar FullcolorBrushMinSize("FullcolorBrushMinSize", 1);
TEnv::IntVar FullcolorBrushMaxSize("FullcolorBrushMaxSize", 5);
TEnv::IntVar FullcolorPressureSensitivity("FullcolorPressureSensitivity", 1);
TEnv::DoubleVar FullcolorBrushHardness("FullcolorBrushHardness", 100);
TEnv::DoubleVar FullcolorMinOpacity("FullcolorMinOpacity", 100);
TEnv::DoubleVar FullcolorMaxOpacity("FullcolorMaxOpacity", 100);
TEnv::DoubleVar FullcolorModifierSize("FullcolorModifierSize", 0);
TEnv::DoubleVar FullcolorModifierOpacity("FullcolorModifierOpacity", 100);
TEnv::IntVar FullcolorModifierEraser("FullcolorModifierEraser", 0);
TEnv::IntVar FullcolorModifierLockAlpha("FullcolorModifierLockAlpha", 0);
TEnv::IntVar FullcolorAssistants("FullcolorAssistants", 1);
TEnv::StringVar FullcolorBrushPreset("FullcolorBrushPreset", "<custom>");
TEnv::StringVar FullcolorBrushTip("FullcolorBrushTip", "<round>");
TEnv::StringVar FullcolorNativeBrush("FullcolorNativeBrush", "<legacy>");

namespace {
QString nativeBrushFolder() {
  return ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
}
class NativeImportProgress final : public QDialog {
  std::atomic_bool &cancelled;
  QLabel *message;
  QPushButton *cancel;

public:
  explicit NativeImportProgress(std::atomic_bool &flag) : cancelled(flag) {
    setWindowTitle(tr("Import Raster Brush"));
    auto layout = new QVBoxLayout(this);
    message = new QLabel(tr("Reading and validating brush resources…"), this);
    layout->addWidget(message);
    auto progress = new QProgressBar(this);
    progress->setRange(0, 0);
    layout->addWidget(progress);
    cancel = new QPushButton(tr("Cancel"), this);
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &NativeImportProgress::reject);
  }
  void reject() override {
    cancelled.store(true, std::memory_order_relaxed);
    message->setText(tr("Cancelling after the current bounded resource…"));
    cancel->setEnabled(false);
    // Keep the dialog modal until the worker finishes. No result can be
    // installed after cancellation, and another import cannot race this one.
  }
};

RasterBrush::ImportResult importNativeBrush(const QString &path,
                                            QString &error) {
  std::atomic_bool cancelled(false);
  NativeImportProgress dialog(cancelled);
  RasterBrush::ImportResult result;
  std::unique_ptr<QThread> worker(QThread::create([&]() {
    try {
      result = RasterBrush::importBrushProcess(
          QCoreApplication::applicationFilePath(), path, error, &cancelled);
    } catch (const std::exception &e) {
      error = QString::fromUtf8(e.what());
    } catch (...) {
      error = QObject::tr("Brush import failed.");
    }
  }));
  QObject::connect(worker.get(), &QThread::finished, &dialog, &QDialog::accept);
  worker->start();
  dialog.exec();
  worker->wait();
  if (cancelled.load(std::memory_order_relaxed)) {
    error.clear();
    return {};
  }
  return result;
}

QStringList installNativeCollection(
    const std::vector<RasterBrush::Record> &records, QString &error) {
  std::atomic_bool cancelled(false);
  NativeImportProgress dialog(cancelled);
  QStringList result;
  const QString directory = nativeBrushFolder();
  std::unique_ptr<QThread> worker(QThread::create([&]() {
    try {
      result =
          RasterBrush::installCollection(directory, records, error, &cancelled);
    } catch (const std::exception &e) {
      error = QString::fromUtf8(e.what());
    } catch (...) {
      error = QObject::tr("Brush installation failed.");
    }
  }));
  QObject::connect(worker.get(), &QThread::finished, &dialog, &QDialog::accept);
  worker->start();
  dialog.exec();
  worker->wait();
  // A nonempty result means the atomic publish already completed. A late
  // cancellation cannot pretend that an installed collection does not exist.
  if (result.empty() && cancelled.load(std::memory_order_relaxed))
    error.clear();
  return result;
}
class NativeBrushPreview final : public QWidget, public RasterBrush::Surface {
  std::vector<RasterBrush::Pixel> pixels;
  std::unique_ptr<RasterBrush::Stroke> stroke;
  RasterBrush::Preset preset;
  double tick = 0;

public:
  explicit NativeBrushPreview(QWidget *parent)
      : QWidget(parent), pixels(320 * 140) {
    setFixedSize(320, 140);
    setMouseTracking(true);
  }
  int width() const override { return 320; }
  int height() const override { return 140; }
  bool prepare(int, int, int, int) override { return true; }
  RasterBrush::Pixel read(int x, int y) const override {
    return pixels[size_t(y) * 320 + x];
  }
  void write(int x, int y, const RasterBrush::Pixel &p) override {
    pixels[size_t(y) * 320 + x] = p;
  }
  void configure(const RasterBrush::Preset &p) {
    preset = p;
    pixels.assign(320 * 140, {});
    stroke.reset();
    RasterBrush::Stroke demo(*this, preset);
    for (int i = 0; i < 240; ++i) {
      RasterBrush::Sample s;
      s.x           = 20 + i;
      s.y           = 70 + std::sin(i * .025) * 25;
      s.time        = i / 240.0;
      s.pressure    = .2 + .8 * std::sin(i / 240.0 * 3.141592653589793);
      s.hasPressure = true;
      demo.add(s);
    }
    demo.finish();
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QImage im(320, 140, QImage::Format_ARGB32_Premultiplied);
    auto q = [](double v) {
      return std::max(0, std::min(255, int(v * 255 + .5)));
    };
    for (int y = 0; y < 140; ++y)
      for (int x = 0; x < 320; ++x) {
        auto p = read(x, y);
        im.setPixel(x, y, qRgba(q(p.r), q(p.g), q(p.b), q(p.a)));
      }
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    painter.drawImage(0, 0, im);
  }
  void mousePressEvent(QMouseEvent *e) override {
    if (e->button() != Qt::LeftButton) return;
    stroke.reset(new RasterBrush::Stroke(*this, preset));
    tick = 0;
    drawAt(e);
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    if (stroke && e->buttons().testFlag(Qt::LeftButton)) drawAt(e);
  }
  void mouseReleaseEvent(QMouseEvent *) override {
    if (stroke) stroke->finish();
    stroke.reset();
  }
  void drawAt(QMouseEvent *e) {
    RasterBrush::Sample s;
    s.x    = e->pos().x();
    s.y    = e->pos().y();
    s.time = tick;
    tick += 1.0 / 120;
    stroke->add(s);
    update();
  }
};

bool editNativeDynamics(QWidget *parent, RasterBrush::Dynamic &dynamic) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QObject::tr("Brush Dynamics"));
  auto layout = new QVBoxLayout(&dialog);
  auto help   = new QLabel(
      QObject::tr(
          "Sensor curves multiply together. Curve points use input:output "
            "pairs between 0 and 1, in increasing input order. Scale normalizes "
            "distance, time, speed and dab index before the curve. Dab index "
            "starts at zero. Initial direction uses the first moving segment; "
            "before movement its value is zero."),
      &dialog);
  help->setWordWrap(true);
  layout->addWidget(help);
  auto minimum = new QDoubleSpinBox(&dialog);
  minimum->setRange(0, 1);
  minimum->setDecimals(4);
  minimum->setValue(dynamic.minimum);
  auto form = new QFormLayout;
  form->addRow(QObject::tr("Minimum fraction"), minimum);
  layout->addLayout(form);
  auto table = new QTableWidget(0, 4, &dialog);
  table->setHorizontalHeaderLabels({QObject::tr("Sensor"), QObject::tr("Scale"),
                                    QObject::tr("Curve points"),
                                    QObject::tr("Interpolation")});
  table->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(table);
  const QStringList sensors = {QObject::tr("Pressure"),
                               QObject::tr("Tilt X"),
                               QObject::tr("Tilt Y"),
                               QObject::tr("Tilt elevation"),
                               QObject::tr("Tilt direction"),
                               QObject::tr("Barrel rotation"),
                               QObject::tr("Stroke direction"),
                               QObject::tr("Distance"),
                               QObject::tr("Time"),
                               QObject::tr("Speed"),
                               QObject::tr("Random per dab"),
                               QObject::tr("Random per stroke"),
                               QObject::tr("Initial stroke direction"),
                               QObject::tr("Dab index")};
  auto add                  = [&](const RasterBrush::Mapping &mapping) {
    int row = table->rowCount();
    table->insertRow(row);
    auto sensor = new QComboBox(table);
    sensor->addItems(sensors);
    sensor->setCurrentIndex(int(mapping.sensor));
    table->setCellWidget(row, 0, sensor);
    table->setItem(
        row, 1, new QTableWidgetItem(QString::number(mapping.scale, 'g', 17)));
    QStringList points;
    for (const auto &k : mapping.curve)
      points << QString::number(k.x, 'g', 17) + ":" +
                    QString::number(k.y, 'g', 17);
    table->setItem(row, 2, new QTableWidgetItem(points.join(", ")));
    auto interpolation = new QComboBox(table);
    interpolation->addItems(
        {QObject::tr("Linear"), QObject::tr("Natural cubic")});
    interpolation->setCurrentIndex(int(mapping.interpolation));
    table->setCellWidget(row, 3, interpolation);
  };
  for (const auto &mapping : dynamic.mappings) add(mapping);
  auto actions = new QHBoxLayout;
  auto plus    = new QPushButton(QObject::tr("Add sensor"), &dialog);
  auto minus = new QPushButton(QObject::tr("Remove selected sensor"), &dialog);
  actions->addWidget(plus);
  actions->addWidget(minus);
  layout->addLayout(actions);
  QObject::connect(plus, &QPushButton::clicked, &dialog, [&]() {
    if (table->rowCount() < 16) add(RasterBrush::Mapping());
  });
  QObject::connect(minus, &QPushButton::clicked, &dialog, [&]() {
    if (table->currentRow() >= 0) table->removeRow(table->currentRow());
  });
  auto buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
    std::vector<RasterBrush::Mapping> mappings;
    bool valid = true;
    for (int row = 0; row < table->rowCount(); ++row) {
      RasterBrush::Mapping m;
      m.sensor = RasterBrush::Sensor(
          static_cast<QComboBox *>(table->cellWidget(row, 0))->currentIndex());
      m.interpolation = RasterBrush::Interpolation(
          static_cast<QComboBox *>(table->cellWidget(row, 3))->currentIndex());
      bool ok = false;
      m.scale = table->item(row, 1)->text().toDouble(&ok);
      valid &=
          ok && std::isfinite(m.scale) && m.scale >= 1e-9 && m.scale <= 1e9;
      m.curve.clear();
      double last = -1;
      for (const auto &point : table->item(row, 2)->text().split(',')) {
        auto pair = point.split(':');
        if (pair.size() != 2) {
          valid = false;
          break;
        }
        bool xok = false, yok = false;
        double x = pair[0].trimmed().toDouble(&xok),
               y = pair[1].trimmed().toDouble(&yok);
        valid &= xok && yok && std::isfinite(x) && std::isfinite(y) &&
                 x > last && x >= 0 && x <= 1 && y >= 0 && y <= 1;
        if (m.interpolation == RasterBrush::Interpolation::NaturalCubic &&
            last >= 0 && x - last < 1e-9)
          valid = false;
        last = x;
        m.curve.push_back({x, y});
      }
      valid &= !m.curve.empty() && m.curve.size() <= 1024;
      mappings.push_back(std::move(m));
    }
    if (!valid) {
      QMessageBox::warning(
          &dialog, QObject::tr("Invalid curve"),
          QObject::tr(
              "Use increasing input:output pairs in the range 0 to 1 and a "
              "positive finite scale. Each sensor allows up to 1024 points."));
      return;
    }
    dynamic.minimum  = minimum->value();
    dynamic.mappings = std::move(mappings);
    dialog.accept();
  });
  dialog.resize(720, 440);
  return dialog.exec() == QDialog::Accepted;
}

bool editNativeBrush(RasterBrush::Record &record) {
  QDialog dialog;
  dialog.setWindowTitle(QObject::tr("Native Raster Brush"));
  auto layout           = new QVBoxLayout(&dialog);
  const bool conversion = record.provenance.contains("sourceSha256");
  const bool candidate = record.preset.profile != "groundzero-native-1";
  auto status           = new QLabel(
      candidate ? QObject::tr("Experimental source profile. Complete source brush behavior is not verified.") : conversion
          ? QObject::tr(
                "Native conversion. Source brush behavior is not verified.")
          : QObject::tr("Native settings supported. External brush parity is "
                                            "not verified."),
      &dialog);
  status->setWordWrap(true);
  layout->addWidget(status);
  auto details = new QPushButton(QObject::tr("Brush details…"), &dialog);
  layout->addWidget(details);
  QObject::connect(details, &QPushButton::clicked, &dialog, [&]() {
    QStringList notes = record.diagnostics;
    const QString previousReport =
        record.provenance.value("conversionReport").toString();
    if (!previousReport.isEmpty() && !notes.join("\n").contains(previousReport))
      notes << previousReport;
    notes << QObject::tr("Behavior profile: %1")
                 .arg(QString::fromStdString(record.preset.profile));
    notes << QObject::tr("Decoded native package: %1")
                 .arg(record.decoded ? QObject::tr("yes") : QObject::tr("no"));
    notes << QObject::tr("Native settings supported: %1")
                 .arg(record.semanticallySupported ? QObject::tr("yes")
                                                   : QObject::tr("no"));
    notes << QObject::tr("Source reference verification: not established");
    QMessageBox report(QMessageBox::Information, QObject::tr("Brush details"),
                       status->text(), QMessageBox::Ok, &dialog);
    report.setTextFormat(Qt::PlainText);
    report.setDetailedText(notes.join("\n"));
    report.exec();
  });
  auto preview = new NativeBrushPreview(&dialog);
  layout->addWidget(preview, 0, Qt::AlignHCenter);
  auto scroll = new QScrollArea(&dialog);
  scroll->setWidgetResizable(true);
  auto controls = new QWidget(scroll);
  auto form     = new QFormLayout(controls);
  scroll->setWidget(controls);
  layout->addWidget(scroll);
  dialog.resize(480, 680);
  auto &p      = record.preset;
  const RasterBrush::Preset originalPreset = p;
  auto refresh = [&]() { preview->configure(p); };
  auto spin = [&](const QString &label, double &value, double lo, double hi) {
    auto box = new QDoubleSpinBox(&dialog);
    box->setDecimals(3);
    box->setRange(lo, hi);
    box->setValue(value);
    form->addRow(label, box);
    double *target = &value;
    QObject::connect(box,
                     static_cast<void (QDoubleSpinBox::*)(double)>(
                         &QDoubleSpinBox::valueChanged),
                     &dialog, [&, target](double v) {
                       *target = v;
                       refresh();
                     });
  };
  spin(QObject::tr("Size"), p.size.value, .25, 4096);
  spin(QObject::tr("Aspect ratio"), p.aspect.value, .01, 1);
  spin(QObject::tr("Angle"), p.angle.value, -360, 360);
  spin(QObject::tr("Spacing"), p.spacing, .01, 5);
  spin(QObject::tr("Hardness"), p.tip.hardness, 0, 1);
  spin(QObject::tr("Opacity"), p.opacity.value, 0, 1);
  spin(QObject::tr("Flow"), p.flow.value, 0, 1);
  spin(QObject::tr("Scatter"), p.scatter.value, 0, 5);
  spin(QObject::tr("Stamp count factor"), p.countControl.value, 0, 1);
  spin(QObject::tr("Airbrush dabs/second"), p.airbrushRate, 0, 120);
  spin(QObject::tr("Paint pickup"), p.pickup, 0, 1);
  spin(QObject::tr("Foreground paint mix"), p.colorRate, 0, 1);
  spin(QObject::tr("Paint reload"), p.reload, 0, 1);
  spin(QObject::tr("Smudge sampling radius"), p.smudgeRadius, 0, 3);
  spin(QObject::tr("Background color mix"), p.backgroundMix.value, 0, 1);
  spin(QObject::tr("Hue rotation"), p.hue.value, -360, 360);
  spin(QObject::tr("Saturation multiplier"), p.saturation.value, 0, 2);
  spin(QObject::tr("Brightness multiplier"), p.brightness.value, 0, 2);
  auto background =
      new QPushButton(QObject::tr("Choose background color…"), &dialog);
  form->addRow(QObject::tr("Color dynamics"), background);
  QObject::connect(background, &QPushButton::clicked, &dialog, [&]() {
    const auto &b = p.background;
    QColor initial =
        QColor::fromRgbF(b.a > 0 ? b.r / b.a : 0, b.a > 0 ? b.g / b.a : 0,
                         b.a > 0 ? b.b / b.a : 0, b.a);
    QColor color = QColorDialog::getColor(initial, &dialog,
                                          QObject::tr("Background paint color"),
                                          QColorDialog::ShowAlphaChannel);
    if (!color.isValid()) return;
    double a     = color.alphaF();
    p.background = {color.redF() * a, color.greenF() * a, color.blueF() * a, a};
    refresh();
  });
  auto dynamics = [&](const QString &label, RasterBrush::Dynamic &dynamic) {
    auto button = new QPushButton(QObject::tr("Edit sensor curves…"), &dialog);
    auto *target = &dynamic;
    form->addRow(label, button);
    QObject::connect(button, &QPushButton::clicked, &dialog, [&, target]() {
      if (editNativeDynamics(&dialog, *target)) refresh();
    });
  };
  dynamics(QObject::tr("Size dynamics"), p.size);
  dynamics(QObject::tr("Aspect dynamics"), p.aspect);
  dynamics(QObject::tr("Angle dynamics"), p.angle);
  dynamics(QObject::tr("Opacity dynamics"), p.opacity);
  dynamics(QObject::tr("Flow dynamics"), p.flow);
  dynamics(QObject::tr("Scatter dynamics"), p.scatter);
  dynamics(QObject::tr("Stamp count dynamics"), p.countControl);
  dynamics(QObject::tr("Pickup dynamics"), p.pickupControl);
  dynamics(QObject::tr("Paint mix dynamics"), p.colorRateControl);
  dynamics(QObject::tr("Smudge radius dynamics"), p.radiusControl);
  dynamics(QObject::tr("Background mix dynamics"), p.backgroundMix);
  dynamics(QObject::tr("Hue dynamics"), p.hue);
  dynamics(QObject::tr("Saturation dynamics"), p.saturation);
  dynamics(QObject::tr("Brightness dynamics"), p.brightness);
  auto check = [&](const QString &label, bool &value) {
    auto box = new QCheckBox(&dialog);
    box->setChecked(value);
    bool *target = &value;
    form->addRow(label, box);
    QObject::connect(box, &QCheckBox::toggled, &dialog, [&, target](bool v) {
      *target = v;
      refresh();
    });
    return box;
  };
  check(QObject::tr("Flip tip horizontally"), p.flipX);
  check(QObject::tr("Flip tip vertically"), p.flipY);
  check(QObject::tr("Preserve alpha"), p.lockAlpha);
  check(QObject::tr("Scatter in both axes"), p.scatterBothAxes);
  auto count = new QSpinBox(&dialog);
  count->setRange(1, 64);
  count->setValue(p.count);
  form->addRow(QObject::tr("Dabs per step"), count);
  QObject::connect(
      count, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
      &dialog, [&](int n) {
        p.count = n;
        refresh();
      });
  auto seed = new QLineEdit(QString::number(qulonglong(p.seed)), &dialog);
  form->addRow(QObject::tr("Random seed"), seed);
  auto blend = new QComboBox(&dialog);
  blend->addItems({QObject::tr("Normal"), QObject::tr("Multiply"),
                   QObject::tr("Screen"), QObject::tr("Overlay"),
                   QObject::tr("Erase")});
  blend->setCurrentIndex(int(p.blend));
  form->addRow(QObject::tr("Blend"), blend);
  QObject::connect(
      blend,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
      &dialog, [&](int n) {
        p.blend = RasterBrush::Blend(n);
        refresh();
      });

  auto readImages = [&](bool sequence) {
    std::vector<std::shared_ptr<const RasterBrush::Image>> images;
    QStringList paths = QFileDialog::getOpenFileNames(
        &dialog, QObject::tr("Choose PNG brush images"), QString(),
        QObject::tr("PNG images (*.png)"));
    if (paths.isEmpty()) return images;
    if ((!sequence && paths.size() != 1) || paths.size() > 256) {
      QMessageBox::warning(&dialog, QObject::tr("Brush images"),
                           sequence ? QObject::tr("Select at most 256 images.")
                                    : QObject::tr("Select one texture image."));
      return images;
    }
    bool ok           = false;
    QStringList modes = {QObject::tr("Dark pixels form the mask"),
                         QObject::tr("Alpha channel forms the mask"),
                         QObject::tr("Preserve image colors")};
    QString selected  = QInputDialog::getItem(
        &dialog, QObject::tr("Image coverage"), QObject::tr("Interpretation"),
        modes, 0, false, &ok);
    if (!ok) return images;
    int mode     = modes.indexOf(selected);
    size_t bytes = 0;
    QSet<const RasterBrush::Image *> retained;
    auto account = [&](const std::shared_ptr<const RasterBrush::Image> &image) {
      if (image && !retained.contains(image.get())) {
        retained.insert(image.get());
        bytes += image->pixels.size() * sizeof(RasterBrush::Pixel);
      }
    };
    for (const auto *preset :
         {&originalPreset, static_cast<const RasterBrush::Preset *>(&p)}) {
      for (const auto &image : preset->tip.images) account(image);
      for (const auto &image : preset->secondary.tip.images) account(image);
      account(preset->texture.image);
    }
    for (const auto &path : paths) {
      QFile file(path);
      QString error;
      std::shared_ptr<const RasterBrush::Image> image;
      if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
        error = QObject::tr("Cannot read image within the 64 MiB file limit.");
      else
        image = RasterBrush::decodeImage(
            file.readAll(), mode != 1, error,
            bytes < 256u * 1024u * 1024u ? 256u * 1024u * 1024u - bytes : 0);
      if (image && bytes + image->pixels.size() * sizeof(RasterBrush::Pixel) >
                       256u * 1024u * 1024u)
        error = QObject::tr("Combined decoded images exceed 256 MiB.");
      if (!error.isEmpty() || !image) {
        QMessageBox::warning(&dialog, QObject::tr("Brush images"), error);
        images.clear();
        return images;
      }
      bytes += image->pixels.size() * sizeof(RasterBrush::Pixel);
      if (mode == 0) {
        // Fresh decode results are uniquely owned until this editor publishes
        // them.
        auto mask = std::const_pointer_cast<RasterBrush::Image>(image);
        image.reset();
        mask->color = false;
        for (auto &pixel : mask->pixels)
          pixel = {0, 0, 0,
                   std::max(0.0, pixel.a - .2126 * pixel.r - .7152 * pixel.g -
                                     .0722 * pixel.b)};
        image = mask;
      }
      images.push_back(image);
    }
    return images;
  };
  auto tipControls = [&](const QString &label, RasterBrush::Tip &tip) {
    auto *target = &tip;
    auto shape   = new QComboBox(&dialog);
    shape->addItems({QObject::tr("Round"), QObject::tr("Square"),
                     QObject::tr("Gaussian"), QObject::tr("Image sequence")});
    shape->setCurrentIndex(int(tip.shape));
    form->addRow(label, shape);
    QObject::connect(
        shape,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        &dialog, [&, target](int n) {
          target->shape = RasterBrush::Shape(n);
          refresh();
        });
    auto images = new QPushButton(QObject::tr("Load image sequence…"), &dialog);
    form->addRow(label + QObject::tr(" images"), images);
    QObject::connect(images, &QPushButton::clicked, &dialog,
                     [&, target, shape]() {
                       auto loaded = readImages(true);
                       if (loaded.empty()) return;
                       target->images = std::move(loaded);
                       target->shape  = RasterBrush::Shape::Bitmap;
                       shape->setCurrentIndex(int(target->shape));
                       refresh();
                     });
    auto clear =
        new QPushButton(QObject::tr("Remove embedded images"), &dialog);
    form->addRow(clear);
    QObject::connect(clear, &QPushButton::clicked, &dialog,
                     [&, target, shape]() {
                       target->images.clear();
                       if (target->shape == RasterBrush::Shape::Bitmap) {
                         target->shape = RasterBrush::Shape::Round;
                         shape->setCurrentIndex(0);
                       }
                       refresh();
                     });
    check(label + QObject::tr(" random sequence"), tip.randomSequence);
    check(label + QObject::tr(" antialias edges"), tip.antialiasEdges);
  };
  tipControls(QObject::tr("Primary tip"), p.tip);
  auto secondary = check(QObject::tr("Enable dual tip"), p.secondary.enabled);
  tipControls(QObject::tr("Secondary tip"), p.secondary.tip);
  spin(QObject::tr("Secondary hardness"), p.secondary.tip.hardness, 0, 1);
  spin(QObject::tr("Secondary relative size"), p.secondary.size.value, .01, 10);
  spin(QObject::tr("Secondary angle"), p.secondary.angle.value, -360, 360);
  spin(QObject::tr("Secondary opacity"), p.secondary.opacity.value, 0, 1);
  spin(QObject::tr("Secondary aspect"), p.secondary.aspect, .01, 1);
  spin(QObject::tr("Secondary scatter"), p.secondary.scatter, 0, 5);
  check(QObject::tr("Flip secondary tip horizontally"), p.secondary.flipX);
  check(QObject::tr("Flip secondary tip vertically"), p.secondary.flipY);
  auto secondaryCount = new QSpinBox(&dialog);
  secondaryCount->setRange(1, 64);
  secondaryCount->setValue(p.secondary.count);
  form->addRow(QObject::tr("Secondary stamps per dab"), secondaryCount);
  QObject::connect(
      secondaryCount, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
      &dialog, [&](int n) {
        p.secondary.count = n;
        refresh();
      });
  dynamics(QObject::tr("Secondary size dynamics"), p.secondary.size);
  dynamics(QObject::tr("Secondary angle dynamics"), p.secondary.angle);
  dynamics(QObject::tr("Secondary opacity dynamics"), p.secondary.opacity);
  auto maskMode = [&](const QString &label, RasterBrush::MaskMode &mode) {
    auto box = new QComboBox(&dialog);
    box->addItems({QObject::tr("Multiply"), QObject::tr("Subtract")});
    box->setCurrentIndex(int(mode));
    auto *target = &mode;
    form->addRow(label, box);
    QObject::connect(
        box,
        static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        &dialog, [&, target](int n) {
          *target = RasterBrush::MaskMode(n);
          refresh();
        });
  };
  maskMode(QObject::tr("Dual tip mask"), p.secondary.mode);
  auto texture = new QPushButton(QObject::tr("Load texture PNG…"), &dialog);
  form->addRow(QObject::tr("Texture"), texture);
  QObject::connect(texture, &QPushButton::clicked, &dialog, [&]() {
    auto images = readImages(false);
    if (!images.empty()) {
      p.texture.image = images.front();
      refresh();
    }
  });
  auto clearTexture = new QPushButton(QObject::tr("Remove texture"), &dialog);
  form->addRow(clearTexture);
  QObject::connect(clearTexture, &QPushButton::clicked, &dialog, [&]() {
    p.texture.image.reset();
    refresh();
  });
  maskMode(QObject::tr("Texture mask"), p.texture.mode);
  spin(QObject::tr("Texture scale"), p.texture.scale, .01, 4096);
  spin(QObject::tr("Texture angle"), p.texture.angle, -360, 360);
  spin(QObject::tr("Texture offset X"), p.texture.offsetX, -1e9, 1e9);
  spin(QObject::tr("Texture offset Y"), p.texture.offsetY, -1e9, 1e9);
  spin(QObject::tr("Texture depth"), p.texture.depth, 0, 1);
  spin(QObject::tr("Texture contrast"), p.texture.contrast, 0, 10);
  spin(QObject::tr("Texture brightness"), p.texture.brightness, -1, 1);
  check(QObject::tr("Invert texture"), p.texture.invert);
  check(QObject::tr("Texture follows each dab"), p.texture.perDab);
  auto wet = new QComboBox(&dialog);
  wet->addItems(
      {QObject::tr("Dry"), QObject::tr("Wet mix"), QObject::tr("Smear")});
  wet->setCurrentIndex(int(p.wet));
  form->addRow(QObject::tr("Paint behavior"), wet);
  auto wash = new QCheckBox(&dialog);
  wash->setChecked(p.wash);
  form->addRow(QObject::tr("Per-stroke opacity (wash)"), wash);
  QObject::connect(
      wet,
      static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
      &dialog, [&](int n) {
        p.wet = RasterBrush::Wet(n);
        if (n) {
          wash->setChecked(false);
          blend->setCurrentIndex(0);
          secondary->setChecked(false);
          p.wash              = false;
          p.blend             = RasterBrush::Blend::Normal;
          p.secondary.enabled = false;
        }
        blend->setEnabled(!n);
        secondary->setEnabled(!n);
        refresh();
      });

  QObject::connect(wash, &QCheckBox::toggled, &dialog, [&](bool v) {
    p.wash = v;
    if (v) {
      p.wet = RasterBrush::Wet::Off;
      wet->setCurrentIndex(0);
    }
    refresh();
  });
  auto buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  blend->setEnabled(p.wet == RasterBrush::Wet::Off);
  secondary->setEnabled(p.wet == RasterBrush::Wet::Off);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
    bool validSeed = false;
    auto number    = seed->text().toULongLong(&validSeed);
    auto why       = RasterBrush::validate(p);
    if (!validSeed || !why.empty()) {
      QMessageBox::warning(
          &dialog, QObject::tr("Invalid brush"),
          validSeed
              ? QString::fromStdString(why)
              : QObject::tr("Random seed must be an unsigned 64-bit integer."));
      return;
    }
    p.seed = number;
    dialog.accept();
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  refresh();
  return dialog.exec() == QDialog::Accepted;
}
}  // namespace

void FullColorBrushTool::initNativePresets() {
  const std::wstring selected =
      QString::fromStdString(FullcolorNativeBrush.getValue()).toStdWString();
  m_nativePreset.deleteAllValues();
  for (const auto &pair : std::vector<std::pair<std::wstring, QString>>{
           {L"<legacy>", tr("Legacy MyPaint")},
           {L"<round>", tr("Native Round")},
           {L"<wet>", tr("Native Wet Mix")},
           {L"<smear>", tr("Native Smear")},
           {L"<browse>", tr("Find Native Brush…")},
           {L"<import>", tr("Import Native Brush…")},
           {L"<edit>", tr("Edit / Save Native Brush…")},
           {L"<export>", tr("Export Native Brush…")}})
    m_nativePreset.addValueWithUIName(pair.first, pair.second);
  QStringList files;
  QDir root(nativeBrushFolder());
  QDirIterator iterator(root.path(), {"*.gzbrush"},
                        QDir::Files | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) files << root.relativeFilePath(iterator.next());
  files.sort();
  for (const auto &file : files) {
    QString name = QFileInfo(file).fileName().section('-', 0, -2);
    if (name.isEmpty()) name = file;
    m_nativePreset.addValueWithUIName(file.toStdWString(), name);
  }
  if (!m_nativePreset.isValue(selected)) {
    m_nativePreset.setValue(L"<legacy>");
    m_nativeRecord.reset();
    if (selected != L"<legacy>")
      DVGui::warning(
          tr("The selected native brush is missing. Reimport it or choose "
             "another brush."));
    FullcolorNativeBrush = "<legacy>";
  } else
    m_nativePreset.setValue(selected);
  if (getApplication() && getApplication()->getCurrentTool())
    getApplication()->getCurrentTool()->notifyToolComboBoxListChanged(
        "Native Brush:");
  selectNativePreset();
}

bool FullColorBrushTool::selectNativePreset() {
  if (m_started) {
    m_nativePreset.setValue(
        QString::fromStdString(FullcolorNativeBrush.getValue()).toStdWString());
    return true;
  }
  QString id = QString::fromStdWString(m_nativePreset.getValue()), error;
  auto previous =
      QString::fromStdString(FullcolorNativeBrush.getValue()).toStdWString();
  if (id == "<browse>") {
    m_nativePreset.setValue(previous);
    QDialog chooser;
    chooser.setWindowTitle(tr("Find Native Brush"));
    auto layout = new QVBoxLayout(&chooser);
    auto search = new QLineEdit(&chooser);
    search->setPlaceholderText(tr("Search brush names or collection paths"));
    auto list = new QListWidget(&chooser);
    layout->addWidget(search);
    layout->addWidget(list);
    QDir root(nativeBrushFolder());
    QDirIterator iterator(root.path(), {"*.gzbrush"},
                          QDir::Files | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
      const QString path = root.relativeFilePath(iterator.next());
      auto item          = new QListWidgetItem(path, list);
      item->setData(Qt::UserRole, path);
      if (path.toStdWString() == previous) list->setCurrentItem(item);
    }
    list->sortItems();
    auto buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &chooser);
    layout->addWidget(buttons);
    QObject::connect(
        search, &QLineEdit::textChanged, &chooser,
        [list](const QString &query) {
          for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(
                !list->item(i)->text().contains(query, Qt::CaseInsensitive));
        });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &chooser,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &chooser,
                     &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &chooser,
                     [&chooser](QListWidgetItem *) { chooser.accept(); });
    chooser.resize(640, 520);
    search->setFocus();
    if (chooser.exec() != QDialog::Accepted || !list->currentItem() ||
        list->currentItem()->isHidden())
      return true;
    const auto chosen =
        list->currentItem()->data(Qt::UserRole).toString().toStdWString();
    if (m_nativePreset.isValue(chosen)) {
      m_nativePreset.setValue(chosen);
      return selectNativePreset();
    }
    DVGui::warning(tr("The brush library changed. Reopen the brush selector."));
    return true;
  }
  if (id == "<import>") {
    QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Import Native Raster Brush"), QString(),
        tr("Raster brushes (*.gzbrush *.abr *.gbr *.gih *.kpp *.bundle)"));
    m_nativePreset.setValue(previous);
    if (path.isEmpty()) return true;
    auto imported = importNativeBrush(path, error);
    if (!error.isEmpty()) {
      DVGui::warning(error);
      return true;
    }
    if (!imported.diagnostics.isEmpty()) {
      QMessageBox report(QMessageBox::Information,
                         tr("Raster Brush Import"),
                         tr("Review each brush’s conversion or experimental profile "
                            "details before installing it."),
                         QMessageBox::Ok | QMessageBox::Cancel);
      report.setDetailedText(imported.diagnostics.join("\n"));
      report.setDefaultButton(QMessageBox::Cancel);
      if (report.exec() != QMessageBox::Ok) return true;
    }
    if (imported.records.empty()) return true;
    std::vector<RasterBrush::Record> selected = imported.records;
    if (imported.records.size() > 1) {
      QDialog chooser;
      chooser.setWindowTitle(tr("Import Brush Collection"));
      auto layout = new QVBoxLayout(&chooser);
      auto list   = new QListWidget(&chooser);
      layout->addWidget(
          new QLabel(tr("Choose the brushes to install together:"), &chooser));
      layout->addWidget(list);
      for (const auto &record : imported.records) {
        auto item = new QListWidgetItem(
            QString::fromStdString(record.preset.name), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
      }
      auto buttons = new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &chooser);
      layout->addWidget(buttons);
      QObject::connect(buttons, &QDialogButtonBox::accepted, &chooser,
                       &QDialog::accept);
      QObject::connect(buttons, &QDialogButtonBox::rejected, &chooser,
                       &QDialog::reject);
      chooser.resize(480, 520);
      if (chooser.exec() != QDialog::Accepted) return true;
      selected.clear();
      for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked)
          selected.push_back(imported.records[size_t(i)]);
      if (selected.empty()) return true;
    }
    auto installed = installNativeCollection(selected, error);
    if (!error.isEmpty()) {
      DVGui::warning(error);
      return true;
    }
    if (installed.empty()) return true;
    FullcolorNativeBrush = QDir(nativeBrushFolder())
                               .relativeFilePath(installed.front())
                               .toStdString();
    initNativePresets();
    return true;
  }
  if (id == "<edit>" || id == "<export>") {
    m_nativePreset.setValue(previous);
    if (!m_nativeRecord) {
      DVGui::warning(tr("Choose a native brush first."));
      return true;
    }
    auto record = *m_nativeRecord;
    if (id == "<edit>") {
      if (!editNativeBrush(record)) return true;
      bool ok      = false;
      QString name = QInputDialog::getText(
          nullptr, tr("Save Native Brush"), tr("Name"), QLineEdit::Normal,
          QString::fromStdString(record.preset.name), &ok);
      if (!ok || name.trimmed().isEmpty()) return true;
      record.preset.name = name.trimmed().toStdString();
      QString path = RasterBrush::install(nativeBrushFolder(), record, error);
      if (error.isEmpty()) {
        FullcolorNativeBrush = QFileInfo(path).fileName().toStdString();
        initNativePresets();
      }
    } else {
      QString path = QFileDialog::getSaveFileName(
          nullptr, tr("Export Native Brush"),
          QString::fromStdString(record.preset.name) + ".gzbrush",
          tr("Native raster brushes (*.gzbrush)"));
      if (path.isEmpty()) return true;
      RasterBrush::save(path, record, error);
    }
    if (!error.isEmpty()) DVGui::warning(error);
    return true;
  }
  if (id == "<legacy>")
    m_nativeRecord.reset();
  else {
    RasterBrush::Record record;
    if (id == "<round>" || id == "<wet>" || id == "<smear>") {
      record.preset.name = id == "<round>" ? "Native Round"
                           : id == "<wet>" ? "Native Wet Mix"
                                           : "Native Smear";
      record.preset.wet  = id == "<round>" ? RasterBrush::Wet::Off
                           : id == "<wet>" ? RasterBrush::Wet::Dulling
                                           : RasterBrush::Wet::Smear;
      record.preset.size.mappings.push_back(
          {RasterBrush::Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
      record.preset.size.minimum = .05;
      record.decoded = record.semanticallySupported = true;
    } else
      record = RasterBrush::load(QDir(nativeBrushFolder()).filePath(id), error);
    if (!error.isEmpty()) {
      m_nativePreset.setValue(previous);
      DVGui::warning(error);
      return true;
    }
    m_nativeRecord = std::make_shared<RasterBrush::Record>(record);
  }
  FullcolorNativeBrush = id.toStdString();
  if (getApplication() && getApplication()->getCurrentTool())
    getApplication()->getCurrentTool()->notifyToolChanged();
  return true;
}

//----------------------------------------------------------------------------------

#define CUSTOM_WSTR L"<custom>"
#define ROUND_TIP_WSTR L"<round>"

//----------------------------------------------------------------------------------

namespace {

// Images of the brush tips: <library>/brushtips/[<folder>/]<name>.png, the
// folders being made by the import of Photoshop brushes (.abr).
QString brushTipsFolder() {
  return ToonzFolder::getLibraryFolder().getQString() + "/brushtips";
}

const int c_maxTips = 2000;

class FullColorBrushUndo final : public ToolUtils::TFullColorRasterUndo {
  TPoint m_offset;
  QString m_id;
  TINT64 m_rasSize;  // bytes of the cached "after" raster

public:
  FullColorBrushUndo(TTileSetFullColor *tileSet, TXshSimpleLevel *level,
                     const TFrameId &frameId, bool isFrameCreated,
                     const TRasterP &ras, const TPoint &offset,
                     bool isLevelCreated = false)
      : ToolUtils::TFullColorRasterUndo(tileSet, level, frameId, isFrameCreated,
                                        isLevelCreated, 0)
      , m_offset(offset)
      , m_rasSize(getRasterMemorySize(ras.getPointer())) {
    static int counter = 0;

    m_id = QString("FullColorBrushUndo") + QString::number(counter++);
    TImageCache::instance()->add(m_id.toStdString(), TRasterImageP(ras));
  }

  ~FullColorBrushUndo() { TImageCache::instance()->remove(m_id); }

  void redo() const override {
    insertLevelAndFrameIfNeeded();

    TRasterImageP image = getImage();
    TRasterP ras        = image->getRaster();

    TRasterImageP srcImg =
        TImageCache::instance()->get(m_id.toStdString(), false);
    ras->copy(srcImg->getRaster(), m_offset);

    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
    notifyImageChanged();
  }

  int getSize() const override {
    return clampSize((TINT64)sizeof(*this) + m_rasSize +
                     ToolUtils::TFullColorRasterUndo::getSize());
  }

  QString getToolName() override { return QString("Raster Brush Tool"); }
  int getHistoryType() override { return HistoryType::BrushTool; }
};

}  // namespace

//************************************************************************
//    FullColor Brush Tool implementation
//************************************************************************

FullColorBrushTool::FullColorBrushTool(std::string name)
    : TTool(name)
    , m_thickness("Size", 1, 1000, 1, 5, false)
    , m_pressure("Pressure", true)
    , m_opacity("Opacity", 0, 100, 100, 100, true)
    , m_hardness("Hardness:", 0, 100, 100)
    , m_modifierSize("ModifierSize", -3, 3, 0, true)
    , m_modifierOpacity("ModifierOpacity", 0, 100, 100, true)
    , m_modifierEraser("ModifierEraser", false)
    , m_modifierLockAlpha("Lock Alpha", false)
    , m_assistants("Assistants", true)
    , m_tip("Tip:")
    , m_preset("Preset:")
    , m_nativePreset("Native Brush:")
    , m_enabledPressure(false)
    , m_minCursorThick(0)
    , m_maxCursorThick(0)
    , m_tileSet(0)
    , m_tileSaver(0)
    , m_notifier(0)
    , m_presetsLoaded(false)
    , m_tipsLoaded(false)
    , m_firstTime(true)
    , m_started(false)
    , m_strokeFrameId()
    , m_strokeLevel() {
  m_nativeAirbrushTimer.setTimerType(Qt::PreciseTimer);
  m_nativeAirbrushTimer.setInterval(8);
  QObject::connect(&m_nativeAirbrushTimer, &QTimer::timeout,
                   &m_nativeAirbrushTimer, [this]() {
                     if (!m_started || m_nativeStrokeFailed) return;
                     const auto tracks = m_inputmanager.getInputTracks();
                     for (const auto &track : tracks)
                       if (!track->finished() && track->size() > 0) {
                         const auto point = track->back();
                         m_inputmanager.trackEvent(
                             track->deviceId, track->touchId, point.position,
                             point.pressure, point.tilt, track->hasPressure,
                             track->hasTilt, false, TToolTimer::ticks(),
                             point.rotation, point.hasRotation);
                       }
                     m_inputmanager.processTracks();
                   });
  bind(TTool::RasterImage | TTool::EmptyTarget);
  m_thickness.setNonLinearSlider();

  m_prop.bind(m_thickness);
  m_prop.bind(m_hardness);
  m_prop.bind(m_opacity);
  m_prop.bind(m_modifierSize);
  m_prop.bind(m_modifierOpacity);
  m_prop.bind(m_modifierEraser);
  m_prop.bind(m_modifierLockAlpha);
  m_prop.bind(m_pressure);
  m_prop.bind(m_assistants);
  m_prop.bind(m_tip);
  m_prop.bind(m_preset);
  m_prop.bind(m_nativePreset);
  m_nativePreset.addValue(L"<legacy>");
  m_nativePreset.setItemUIName(L"<legacy>", tr("Legacy MyPaint"));

  m_inputmanager.setHandler(this);
#ifndef NDEBUG
  m_modifierTest = new TModifierTest();
#endif
  m_modifierLine         = new TModifierLine();
  m_modifierTangents     = new TModifierTangents();
  m_modifierAssistants   = new TModifierAssistants();
  m_modifierSegmentation = new TModifierSegmentation();

  m_inputmanager.addModifier(
      TInputModifierP(m_modifierAssistants.getPointer()));

  m_thickness.setNonLinearSlider();
  m_preset.setId("BrushPreset");
  m_tip.addValue(ROUND_TIP_WSTR);
  m_tip.setItemUIName(ROUND_TIP_WSTR, tr("Round"));
  m_modifierEraser.setId("RasterEraser");
  m_modifierLockAlpha.setId("LockAlpha");
  m_pressure.setId("PressureSensitivity");
}

//-------------------------------------------------------------------------------------------------------

unsigned int FullColorBrushTool::getToolHints() const {
  unsigned int h = TTool::getToolHints() & ~HintAssistantsAll;
  if (m_assistants.getValue()) {
    h |= HintReplicators;
    h |= HintReplicatorsPoints;
    h |= HintReplicatorsEnabled;
  }
  return h;
}

//---------------------------------------------------------------------------------------------------

ToolOptionsBox *FullColorBrushTool::createOptionsBox() {
  TPaletteHandle *currPalette =
      TTool::getApplication()->getPaletteController()->getCurrentLevelPalette();
  ToolHandle *currTool = TTool::getApplication()->getCurrentTool();
  return new BrushToolOptionsBox(0, this, currPalette, currTool);
}

//---------------------------------------------------------------------------------------------------

void FullColorBrushTool::onCanvasSizeChanged() {
  onDeactivate();
  setWorkAndBackupImages();
}

//---------------------------------------------------------------------------------------------------

void FullColorBrushTool::onColorStyleChanged() {
  getApplication()->getCurrentTool()->notifyToolChanged();
}

//---------------------------------------------------------------------------------------------------

void FullColorBrushTool::updateTranslation() {
  m_thickness.setQStringName(tr("Size"));
  m_pressure.setQStringName(tr("Pressure"));
  m_opacity.setQStringName(tr("Opacity"));
  m_hardness.setQStringName(tr("Hardness:"));
  m_preset.setQStringName(tr("Preset:"));
  m_tip.setQStringName(tr("Tip:"));
  m_modifierSize.setQStringName(tr("Size"));
  m_modifierOpacity.setQStringName(tr("Opacity"));
  m_modifierEraser.setQStringName(tr("Eraser"));
  m_modifierLockAlpha.setQStringName(tr("Lock Alpha"));
  m_assistants.setQStringName(tr("Assistants"));
}

//---------------------------------------------------------------------------------------------------

void FullColorBrushTool::onActivate() {
  if (!m_notifier) m_notifier = new FullColorBrushToolNotifier(this);
  m_notifier->onActivate();

  updateCurrentStyle();

  if (m_firstTime) {
    m_firstTime = false;

    std::wstring wpreset =
        QString::fromStdString(FullcolorBrushPreset.getValue()).toStdWString();
    if (wpreset != CUSTOM_WSTR) {
      initPresets();
      if (!m_preset.isValue(wpreset)) wpreset = CUSTOM_WSTR;
      m_preset.setValue(wpreset);
      FullcolorBrushPreset = m_preset.getValueAsString();
      loadPreset();
    } else
      loadLastBrush();
  }

  initTips();  // picks up the tips imported since the last time
  initNativePresets();
  setWorkAndBackupImages();
  onColorStyleChanged();
  updateModifiers();
  m_skipStrokeUntilDown = false;
}

//--------------------------------------------------------------------------------------------------

void FullColorBrushTool::onDeactivate() {
  if (m_notifier) m_notifier->onDeactivate();

  m_inputmanager.finishTracks();
  m_workRaster          = TRaster32P();
  m_backUpRas           = TRasterP();
  m_skipStrokeUntilDown = false;
}

//--------------------------------------------------------------------------------------------------

void FullColorBrushTool::updateWorkAndBackupRasters(const TRect &rect) {
  if (rect.isEmpty()) return;

  TRasterImageP ri = TImageP(getImage(false, 1));
  if (!ri) return;

  TRasterP ras = ri->getRaster();
  if (!ras || !m_workRaster || !m_backUpRas) return;

  const int denominator = 8;
  TRect enlargedRect    = rect + m_lastRect;
  int dx                = (enlargedRect.getLx() - 1) / denominator + 1;
  int dy                = (enlargedRect.getLy() - 1) / denominator + 1;

  if (m_lastRect.isEmpty()) {
    enlargedRect.x0 -= dx;
    enlargedRect.y0 -= dy;
    enlargedRect.x1 += dx;
    enlargedRect.y1 += dy;

    TRect _rect = enlargedRect * ras->getBounds();
    if (_rect.isEmpty()) return;

    m_workRaster->extract(_rect)->copy(ras->extract(_rect));
    m_backUpRas->extract(_rect)->copy(ras->extract(_rect));
  } else {
    if (enlargedRect.x0 < m_lastRect.x0) enlargedRect.x0 -= dx;
    if (enlargedRect.y0 < m_lastRect.y0) enlargedRect.y0 -= dy;
    if (enlargedRect.x1 > m_lastRect.x1) enlargedRect.x1 += dx;
    if (enlargedRect.y1 > m_lastRect.y1) enlargedRect.y1 += dy;

    TRect _rect = enlargedRect * ras->getBounds();
    if (_rect.isEmpty()) return;

    TRect _lastRect    = m_lastRect * ras->getBounds();
    QList<TRect> rects = ToolUtils::splitRect(_rect, _lastRect);
    for (int i = 0; i < rects.size(); i++) {
      m_workRaster->extract(rects[i])->copy(ras->extract(rects[i]));
      m_backUpRas->extract(rects[i])->copy(ras->extract(rects[i]));
    }
  }

  m_lastRect = enlargedRect;
}

//--------------------------------------------------------------------------------------------------

bool FullColorBrushTool::askRead(const TRect &rect) { return askWrite(rect); }

//--------------------------------------------------------------------------------------------------

bool FullColorBrushTool::askWrite(const TRect &rect) {
  if (rect.isEmpty()) return true;
  if (!m_tileSaver || !m_workRaster) return false;
  m_strokeRect += rect;
  m_strokeSegmentRect += rect;
  updateWorkAndBackupRasters(rect);
  m_tileSaver->save(rect);
  return true;
}

//--------------------------------------------------------------------------------------------------

void FullColorBrushTool::updateModifiers() {
  m_modifierAssistants->magnetism = m_assistants.getValue() ? 1 : 0;
  m_inputmanager.drawPreview      = false; //!m_modifierAssistants->drawOnly;

  m_modifierReplicate.clear();
  if (m_assistants.getValue())
    TReplicator::scanReplicators(this, nullptr, &m_modifierReplicate, false, true, false, false, nullptr);
  
  m_inputmanager.clearModifiers();
  m_inputmanager.addModifier(TInputModifierP(m_modifierTangents.getPointer()));
  m_inputmanager.addModifier(
      TInputModifierP(m_modifierAssistants.getPointer()));
#ifndef NDEBUG
  m_inputmanager.addModifier(TInputModifierP(m_modifierTest.getPointer()));
#endif
  m_inputmanager.addModifiers(m_modifierReplicate);
  m_inputmanager.addModifier(
      TInputModifierP(m_modifierSegmentation.getPointer()));
}

//--------------------------------------------------------------------------------------------------

bool FullColorBrushTool::preLeftButtonDown() {
  if (m_nativeRecord) {
    TRasterImageP existing = TRasterImageP(getImage(false));
    if (existing && existing->getRaster() &&
        !TRaster32P(existing->getRaster())) {
      DVGui::warning(
          tr("Native raster brushes require an 8-bit RGBA raster. The drawing "
             "was not converted."));
      return false;
    }
  }
  updateModifiers();
  touchImage();

  if (m_isFrameCreated) {
    setWorkAndBackupImages();
    // When the xsheet frame is selected, whole viewer will be updated from
    // SceneViewer::onXsheetChanged() on adding a new frame.
    // We need to take care of a case when the level frame is selected.
    if (m_application->getCurrentFrame()->isEditingLevel()) invalidate();
  }

  return true;
}

//---------------------------------------------------------------------------------------------------

void FullColorBrushTool::handleMouseEvent(MouseEventType type,
                                          const TPointD &pos,
                                          const TMouseEvent &e) {
  TTimerTicks t = TToolTimer::ticks();
  bool alt      = e.getModifiersMask() & TMouseEvent::ALT_KEY;
  bool shift    = e.getModifiersMask() & TMouseEvent::SHIFT_KEY;
  bool control  = e.getModifiersMask() & TMouseEvent::CTRL_KEY;

  if (shift && type == ME_DOWN && e.button() == Qt::LeftButton && !m_started) {
    m_modifierAssistants->magnetism = 0;
    m_inputmanager.clearModifiers();
    m_inputmanager.addModifier(TInputModifierP(m_modifierLine.getPointer()));
    m_inputmanager.addModifier(
        TInputModifierP(m_modifierAssistants.getPointer()));
    m_inputmanager.addModifiers(m_modifierReplicate);
    m_inputmanager.addModifier(
        TInputModifierP(m_modifierSegmentation.getPointer()));
    m_inputmanager.drawPreview = true;
  }

  if (alt != m_inputmanager.state.isKeyPressed(TKey::alt))
    m_inputmanager.keyEvent(alt, TKey::alt, t, nullptr);
  if (shift != m_inputmanager.state.isKeyPressed(TKey::shift))
    m_inputmanager.keyEvent(shift, TKey::shift, t, nullptr);
  if (control != m_inputmanager.state.isKeyPressed(TKey::control))
    m_inputmanager.keyEvent(control, TKey::control, t, nullptr);

  if (m_skipStrokeUntilDown) {
    if (type == ME_DOWN)
      m_skipStrokeUntilDown = false;
    else if (type != ME_MOVE)
      return;
  }

  if (type == ME_MOVE) {
    THoverList hovers(1, pos);
    m_inputmanager.hoverEvent(hovers);
  } else {
    TColorStyle *levelStyle = getApplication()->getCurrentLevelStyle();
    bool isMyPaint          = levelStyle && levelStyle->getTagId() == 4001;
    int    deviceId    = e.isTablet() ? 1 : 0;
    double defPressure = isMyPaint ? 0.5 : 1.0;
    bool   hasPressure = e.isTablet();
    double pressure    = hasPressure ? e.m_pressure : defPressure;
    bool   final       = type == ME_UP;
    TPointD tilt;
    double rotation = e.m_rotation;
    if (m_nativeRecord && getViewer()) {
      TAffine orientation = TTrackTransform::makeTiltTransform(
          getViewer()->getViewMatrix().inv());
      tilt = orientation * e.m_tilt;
      TPointD axis =
          orientation * TPointD(std::cos(rotation), std::sin(rotation));
      rotation = std::atan2(axis.y, axis.x);
    }
    m_inputmanager.trackEvent(deviceId, 0, pos, pressure, tilt, hasPressure,
                              m_nativeRecord && e.m_hasTilt, final, t,
                              m_nativeRecord ? rotation : 0,
                              m_nativeRecord && e.m_hasRotation);
    m_inputmanager.processTracks();
  }
}

//---------------------------------------------------------------------------------------------------

void FullColorBrushTool::leftButtonDown(const TPointD &pos,
                                        const TMouseEvent &e) {
  handleMouseEvent(ME_DOWN, pos, e);
}
void FullColorBrushTool::leftButtonDrag(const TPointD &pos,
                                        const TMouseEvent &e) {
  handleMouseEvent(ME_DRAG, pos, e);
}
void FullColorBrushTool::leftButtonUp(const TPointD &pos,
                                      const TMouseEvent &e) {
  handleMouseEvent(ME_UP, pos, e);
}
void FullColorBrushTool::mouseMove(const TPointD &pos, const TMouseEvent &e) {
  handleMouseEvent(ME_MOVE, pos, e);
}

//---------------------------------------------------------------------------------------------------------------

void FullColorBrushTool::inputMouseMove(const TPointD &position,
                                        const TInputState &state) {
  struct Locals {
    FullColorBrushTool *m_this;

    void notify(TProperty &prop) {
      m_this->onPropertyChanged(prop.getName());
      TTool::getApplication()->getCurrentTool()->notifyToolChanged();
    }

    void addMinMax(TIntPairProperty &prop, double add) {
      const TIntPairProperty::Range &range = prop.getRange();

      TIntPairProperty::Value value = prop.getValue();
      value.second =
          tcrop<double>(value.second + add, range.first, range.second);
      value.first = tcrop<double>(value.first + add, range.first, range.second);
      prop.setValue(value);

      notify(prop);
    }

    void addMinMaxSeparate(TIntPairProperty &prop, double min, double max) {
      if (min == 0.0 && max == 0.0) return;
      const TIntPairProperty::Range &range = prop.getRange();

      TIntPairProperty::Value value = prop.getValue();
      value.first += min;
      value.second += max;
      if (value.first > value.second) value.first = value.second;
      value.first = tcrop<double>(value.first, range.first, range.second);

      value.second = tcrop<double>(value.second, range.first, range.second);
      prop.setValue(value);

      notify(prop);
    }

    void add(TDoubleProperty &prop, double x) {
      if (x == 0.0) return;

      const TDoubleProperty::Range &range = prop.getRange();
      double value =
          tcrop<double>(prop.getValue() + x, range.first, range.second);
      prop.setValue(value);

      notify(prop);
    }
  } locals = {this};

  const bool resizeBrush =
      Preferences::instance()->useCtrlAltToResizeBrushEnabled() &&
      state.isKeyPressed(TKey::control) && state.isKeyPressed(TKey::alt) &&
      !state.isKeyPressed(TKey::shift);

  if (resizeBrush) {
    const TPointD &diff = position - m_mousePos;
    if (getBrushStyle()) {
      locals.add(m_modifierSize, 0.01 * diff.x);
    } else {
      locals.addMinMaxSeparate(m_thickness, int(diff.x / 2), int(diff.y / 2));
    }
  } else {
    m_brushPos = position;
  }

  m_mousePos = position;
  invalidate();
}

//-------------------------------------------------------------------------------------------------------------

void FullColorBrushTool::inputSetBusy(bool busy) {
  if (m_skipStrokeUntilDown && busy) return;
  if (m_started == busy) return;
  if (busy) {
    m_nativeStrokeFailed = false;
    m_nativeCreatedFrame = m_isFrameCreated;
    m_nativeCreatedLevel = m_isLevelCreated;
    // begin paint
    TRasterImageP ri = (TRasterImageP)getImage(true);
    if (!ri) return;// ri = (TRasterImageP)touchImage(); touch in preLeftButtonDown
    TRasterP ras = ri->getRaster();
    if (!ras) return;
    if (m_nativeRecord && !TRaster32P(ras)) {
      DVGui::warning(
          tr("Native raster brushes require an 8-bit RGBA raster. The drawing "
             "was not converted."));
      return;
    }

    if (!(m_workRaster && m_backUpRas)) setWorkAndBackupImages();
    if (!m_workRaster) return;
    m_workRaster->lock();
    m_tileSet       = new TTileSetFullColor(ras->getSize());
    m_tileSaver     = new TTileSaverFullColor(ras, m_tileSet);
    if (m_nativeRecord) {
      m_nativeMemory =
          std::make_shared<RasterBrush::MemoryBudget>(256u * 1024u * 1024u);
      auto backing =
          std::make_shared<NativeRasterBrushSurface>(m_workRaster, *this);
      m_nativeSurface = std::make_shared<RasterBrush::BufferedSurface>(
          backing, m_nativeMemory);
    }
    m_strokeFrameId = getCurrentFid();
    m_strokeLevel   = TXshSimpleLevelP();
    if (TTool::Application *app = TTool::getApplication()) {
      if (TXshLevelHandle *lh = app->getCurrentLevel()) {
        if (TXshLevel *xl = lh->getLevel())
          m_strokeLevel = xl->getSimpleLevel();
      }
    }

    // update color here since the current style might be switched
    // with numpad shortcut keys
    updateCurrentStyle();
  } else {
    commitStroke();
  }
  m_started = busy;
  if (busy && m_nativeRecord && m_nativeRecord->preset.airbrushRate > 0)
    m_nativeAirbrushTimer.start();
}

//-------------------------------------------------------------------------------------------------------------

void FullColorBrushTool::inputPaintTrackPoint(const TTrackPoint &point,
                                              const TTrack &track,
                                              bool firstTrack,
                                              bool preview)
{
  // get raster
  if (!m_started || !getViewer() || preview || m_nativeStrokeFailed) return;

  TRasterImageP ri = (TRasterImageP)getImage(true);
  if (!ri) return;
  TRasterP ras = ri->getRaster();
  if (!ras) return;
  TPointD rasCenter = ras->getCenterD();

  // init brush
  TrackHandler *handler;
  if (track.size() == track.pointsAdded && !track.handler && m_workRaster) {
    mypaint::Brush mypaintBrush;
    applyToonzBrushSettings(mypaintBrush);
    handler = new TrackHandler(m_workRaster, *this, mypaintBrush);
    handler->brush.setTip(m_tipImage);
    handler->brush.beginStroke();
    if (m_nativeRecord) {
      RasterBrush::Preset preset = m_nativeRecord->preset;
      const double alpha         = m_currentColor.m / 255.0;
      preset.color               = {m_currentColor.r / 255.0 * alpha,
                                    m_currentColor.g / 255.0 * alpha,
                                    m_currentColor.b / 255.0 * alpha, alpha};
      preset.size.value *= std::pow(2.0, m_modifierSize.getValue());
      preset.opacity.value *= m_modifierOpacity.getValue() / 100.0;
      preset.lockAlpha = m_modifierLockAlpha.getValue();
      if (m_modifierEraser.getValue()) {
        preset.blend = RasterBrush::Blend::Erase;
        preset.wet   = RasterBrush::Wet::Off;
      }
      handler->nativeSurface = m_nativeSurface;
      handler->nativeStroke.reset(
          new RasterBrush::Stroke(*handler->nativeSurface, preset,
                                  256u * 1024u * 1024u, m_nativeMemory));
    }
    track.handler = handler;
  }
  handler = dynamic_cast<TrackHandler *>(track.handler.getPointer());
  if (!handler) return;

  TColorStyle *levelStyle = getApplication()->getCurrentLevelStyle();
  bool isMyPaint          = levelStyle && levelStyle->getTagId() == 4001;
  double defPressure = isMyPaint ? 0.5 : 1.0;
  double pressure    = m_enabledPressure ? point.pressure : defPressure;
  
  // paint stroke
  m_strokeSegmentRect.empty();
  if (handler->nativeStroke) {
    RasterBrush::Sample sample;
    sample.x           = point.position.x + rasCenter.x;
    sample.y           = ras->getLy() - 1 - (point.position.y + rasCenter.y);
    sample.pressure    = pressure;
    sample.hasPressure = m_enabledPressure && track.hasPressure;
    sample.tiltX       = point.tilt.x;
    sample.tiltY       = -point.tilt.y;
    sample.hasTilt  = track.hasTilt || point.tilt.x != 0 || point.tilt.y != 0;
    sample.rotation    = -point.rotation;
    sample.hasRotation = point.hasRotation;
    sample.time        = point.time;
    if (!handler->nativeStroke->add(sample)) {
      // Abort atomically: restore every region touched by this stroke. Do not
      // publish a half-rendered stroke when a resource budget is exhausted.
      if (!m_strokeRect.isEmpty()) {
        TRect restore = m_strokeRect * ras->getBounds();
        ras->extract(restore)->copy(m_backUpRas->extract(restore));
        m_workRaster->extract(restore)->copy(m_backUpRas->extract(restore));
      }
      m_nativeStrokeFailed = true;
      DVGui::warning(QString::fromStdString(handler->nativeStroke->error()));
      handler->nativeStroke->finish();
      return;
    }
    if (track.pointsAdded == 1 && track.finished())
      handler->nativeStroke->finish();
  } else {
    handler->brush.strokeTo(point.position + rasCenter, pressure, point.tilt,
                            point.time - track.previous().time);
    if (track.pointsAdded == 1 && track.finished()) handler->brush.endStroke();
  }

  // update affected area
  TRect updateRect = m_strokeSegmentRect * ras->getBounds();
  if (!updateRect.isEmpty())
    ras->extract(updateRect)->copy(m_workRaster->extract(updateRect));
  TRectD invalidateRect = convert(m_strokeSegmentRect) - rasCenter;
  if (firstTrack) {
    TPointD thickOffset(m_maxCursorThick * 0.5, m_maxCursorThick * 0.5);
    invalidateRect +=
        TRectD(m_brushPos - thickOffset, m_brushPos + thickOffset);
    invalidateRect +=
        TRectD(point.position - thickOffset, point.position + thickOffset);
    m_brushPos = m_mousePos = point.position;
  }
  invalidate(invalidateRect.enlarge(2.0));
}

//-------------------------------------------------------------------------------------------------------------

void FullColorBrushTool::inputInvalidateRect(const TRectD &bounds) {
  invalidate(bounds);
}

//-------------------------------------------------------------------------------------------------------------

void FullColorBrushTool::draw() {
  if (TRasterImageP ri = TRasterImageP(getImage(false))) {
    // If toggled off, don't draw brush outline
    if (!Preferences::instance()->isCursorOutlineEnabled()) return;

    TRasterP ras = ri->getRaster();
    if (!ras) return;

    double alpha       = 1.0;
    double alphaRadius = 3.0;
    double pixelSize   = sqrt(tglGetPixelSize2());

    // circles with lesser radius looks more bold
    // to avoid these effect we'll reduce alpha for small radiuses
    double minX     = m_minCursorThick / (alphaRadius * pixelSize);
    double maxX     = m_maxCursorThick / (alphaRadius * pixelSize);
    double minAlpha = alpha * (1.0 - 1.0 / (1.0 + minX));
    double maxAlpha = alpha * (1.0 - 1.0 / (1.0 + maxX));

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    tglEnableBlending();
    tglEnableLineSmooth(true, 0.5);

    if (m_minCursorThick < m_maxCursorThick - pixelSize) {
      glColor4d(1.0, 1.0, 1.0, minAlpha);
      tglDrawCircle(m_brushPos, (m_minCursorThick + 1) * 0.5 - pixelSize);
      glColor4d(0.0, 0.0, 0.0, minAlpha);
      tglDrawCircle(m_brushPos, (m_minCursorThick + 1) * 0.5);
    }

    glColor4d(1.0, 1.0, 1.0, maxAlpha);
    tglDrawCircle(m_brushPos, (m_maxCursorThick + 1) * 0.5 - pixelSize);
    glColor4d(0.0, 0.0, 0.0, maxAlpha);
    tglDrawCircle(m_brushPos, (m_maxCursorThick + 1) * 0.5);

    glPopAttrib();
  }
  m_inputmanager.draw();
}

//--------------------------------------------------------------------------------------------------------------

void FullColorBrushTool::onEnter() { updateCurrentStyle(); }

//----------------------------------------------------------------------------------------------------------

void FullColorBrushTool::onLeave() {
  m_minCursorThick = 0;
  m_maxCursorThick = 0;
}

//----------------------------------------------------------------------------------------------------------

TPropertyGroup *FullColorBrushTool::getProperties(int targetType) {
  if (!m_presetsLoaded) initPresets();
  if (!m_tipsLoaded) initTips();
  return &m_prop;
}

//----------------------------------------------------------------------------------------------------------

bool FullColorBrushTool::keyDown(QKeyEvent *event) {
  if (event->key() != Qt::Key_Escape || !m_nativeRecord || !m_started)
    return false;
  m_nativeStrokeFailed  = true;
  m_skipStrokeUntilDown = true;
  // Reset flushes tracks, whose paint callbacks now ignore the cancelled
  // stroke.
  m_inputmanager.reset();
  if (m_started) commitStroke();
  invalidate();
  return true;
}

void FullColorBrushTool::commitStroke() {
  m_nativeAirbrushTimer.stop();
  if (!m_started) return;

  TFrameId frameId =
      m_strokeFrameId.isEmptyFrame() ? getCurrentFid() : m_strokeFrameId;
  TXshSimpleLevel *sl = m_strokeLevel.getPointer();

  TRasterImageP ri =
      sl ? TRasterImageP(sl->getFrame(frameId, true)) : TRasterImageP();
  TRasterP ras = ri ? ri->getRaster() : TRasterP();

  std::unique_ptr<FullColorBrushUndo> rollback;
  m_lastRect.empty();
  if (m_workRaster) m_workRaster->unlock();

  if (ras && sl && m_tileSet && m_tileSet->getTileCount() > 0 &&
      !m_nativeStrokeFailed) {
    delete m_tileSaver;
    m_tileSaver     = 0;
    TRasterP subras = ras->extract(m_strokeRect)->clone();
    TUndoManager::manager()->add(new FullColorBrushUndo(
        m_tileSet, sl, frameId,
        m_nativeRecord ? m_nativeCreatedFrame : m_isFrameCreated, subras,
        m_strokeRect.getP00(), m_nativeRecord && m_nativeCreatedLevel));
    m_tileSet = 0;
  } else {
    delete m_tileSaver;
    m_tileSaver = 0;
    if (m_nativeStrokeFailed && ras && sl && m_tileSet) {
      TRect rollbackSample(0, 0, 0, 0);
      rollback.reset(
          new FullColorBrushUndo(m_tileSet, sl, frameId, m_nativeCreatedFrame,
                                 ras->extract(rollbackSample)->clone(),
                                 TPoint(), m_nativeCreatedLevel));
      m_tileSet = 0;
    }
    delete m_tileSet;
    m_tileSet = 0;
  }

  m_started       = false;
  m_nativeSurface.reset();
  m_nativeMemory.reset();
  m_strokeFrameId = TFrameId();
  m_strokeLevel   = TXshSimpleLevelP();
  m_strokeRect.empty();
  if (rollback) {
    rollback->undo();
    setWorkAndBackupImages();
  } else if (ras && sl)
    notifyImageChanged(frameId, sl);
}

//----------------------------------------------------------------------------------------------------------

void FullColorBrushTool::onImageChanged() {
  m_inputmanager.reset();
  if (m_started) {
    commitStroke();
    m_skipStrokeUntilDown = true;
  }
  setWorkAndBackupImages();
}

//----------------------------------------------------------------------------------------------------------

void FullColorBrushTool::setWorkAndBackupImages() {
  TRasterImageP ri = (TRasterImageP)getImage(false, 1);
  if (!ri) return;

  TRasterP ras = ri->getRaster();
  if (!ras) return;
  TDimension dim = ras->getSize();

  if (!m_workRaster || m_workRaster->getLx() != dim.lx ||
      m_workRaster->getLy() != dim.ly)
    m_workRaster = TRaster32P(dim);

  if (!m_backUpRas || m_backUpRas->getLx() != dim.lx ||
      m_backUpRas->getLy() != dim.ly ||
      m_backUpRas->getPixelSize() != ras->getPixelSize())
    m_backUpRas = ras->create(dim.lx, dim.ly);

  m_strokeRect.empty();
  m_lastRect.empty();
}

//------------------------------------------------------------------

bool FullColorBrushTool::onPropertyChanged(std::string propertyName) {
  if (m_propertyUpdating) return true;
  if (propertyName == "Native Brush:") return selectNativePreset();

  updateCurrentStyle();

  if (propertyName == "Tip:") {
    // the tip is not a part of the presets
    FullcolorBrushTip = QString::fromStdWString(m_tip.getValue()).toStdString();
    updateTip();
    return true;
  }

  if (propertyName == "Preset:") {
    if (m_preset.getValue() != CUSTOM_WSTR)
      loadPreset();
    else  // Chose <custom>, go back to last saved brush settings
      loadLastBrush();

    FullcolorBrushPreset = m_preset.getValueAsString();
    m_propertyUpdating   = true;
    getApplication()->getCurrentTool()->notifyToolChanged();
    m_propertyUpdating = false;
    return true;
  }

  FullcolorBrushMinSize        = m_thickness.getValue().first;
  FullcolorBrushMaxSize        = m_thickness.getValue().second;
  FullcolorPressureSensitivity = m_pressure.getValue();
  FullcolorBrushHardness       = m_hardness.getValue();
  FullcolorMinOpacity          = m_opacity.getValue().first;
  FullcolorMaxOpacity          = m_opacity.getValue().second;
  FullcolorModifierSize        = m_modifierSize.getValue();
  FullcolorModifierOpacity     = m_modifierOpacity.getValue();
  FullcolorModifierEraser      = m_modifierEraser.getValue() ? 1 : 0;
  FullcolorModifierLockAlpha   = m_modifierLockAlpha.getValue() ? 1 : 0;
  FullcolorAssistants          = m_assistants.getValue() ? 1 : 0;

  if (m_preset.getValue() != CUSTOM_WSTR) {
    m_preset.setValue(CUSTOM_WSTR);
    FullcolorBrushPreset = m_preset.getValueAsString();
    m_propertyUpdating   = true;
    getApplication()->getCurrentTool()->notifyToolChanged();
    m_propertyUpdating = false;
  }

  return true;
}

//------------------------------------------------------------------

void FullColorBrushTool::initPresets() {
  if (!m_presetsLoaded) {
    // If necessary, load the presets from file
    m_presetsLoaded = true;
    m_presetsManager.load(TEnv::getConfigDir() + "brush_raster.txt");
  }

  // Rebuild the presets property entries
  const std::set<BrushData> &presets = m_presetsManager.presets();

  m_preset.deleteAllValues();
  m_preset.addValue(CUSTOM_WSTR);
  m_preset.setItemUIName(CUSTOM_WSTR, tr("<custom>"));

  std::set<BrushData>::const_iterator it, end = presets.end();
  for (it = presets.begin(); it != end; ++it) m_preset.addValue(it->m_name);
}

//----------------------------------------------------------------------------------------------------------

// Lists the tip images of the library into the "Tip:" property. The tip of
// the last session is selected the first time.
void FullColorBrushTool::initTips() {
  QString folder = brushTipsFolder();
  QStringList files;
  QDirIterator it(folder, QStringList() << "*.png", QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext() && files.size() < c_maxTips) {
    it.next();
    files << QDir(folder).relativeFilePath(it.filePath());
  }
  files.sort(Qt::CaseInsensitive);

  bool first   = !m_tipsLoaded;
  m_tipsLoaded = true;
  // Compared item by item: QList::operator== of Qt 5.15 uses
  // stdext::make_checked_array_iterator on MSVC, which recent MSVC removed
  if (!first && files.size() == m_tipFiles.size() &&
      std::equal(files.begin(), files.end(), m_tipFiles.begin()))
    return;
  m_tipFiles = files;

  std::wstring current =
      first
          ? QString::fromStdString(FullcolorBrushTip.getValue()).toStdWString()
          : m_tip.getValue();

  m_tip.deleteAllValues();
  m_tip.addValue(ROUND_TIP_WSTR);
  m_tip.setItemUIName(ROUND_TIP_WSTR, tr("Round"));
  for (const QString &file : files) {
    QString name = file;
    if (name.endsWith(".png", Qt::CaseInsensitive)) name.chop(4);
    m_tip.addValueWithUIName(file.toStdWString(), name);
  }
  if (!m_tip.isValue(current)) current = ROUND_TIP_WSTR;
  m_tip.setValue(current);
  updateTip();

  if (!first && getApplication() && getApplication()->getCurrentTool())
    getApplication()->getCurrentTool()->notifyToolComboBoxListChanged("Tip:");
}

//----------------------------------------------------------------------------------------------------------

// Loads the image of the selected tip (none for the round dab).
void FullColorBrushTool::updateTip() {
  m_tipImage.reset();
  std::wstring id = m_tip.getValue();
  if (id.empty() || id == ROUND_TIP_WSTR) return;
  m_tipImage =
      MyPaintTip::load(brushTipsFolder() + "/" + QString::fromStdWString(id));
}

//----------------------------------------------------------------------------------------------------------

void FullColorBrushTool::loadPreset() {
  const std::set<BrushData> &presets = m_presetsManager.presets();
  std::set<BrushData>::const_iterator it;

  it = presets.find(BrushData(m_preset.getValue()));
  if (it == presets.end()) return;

  const BrushData &preset = *it;

  try  // Don't bother with RangeErrors
  {
    m_thickness.setValue(TIntPairProperty::Value(std::max((int)preset.m_min, 1),
                                                 (int)preset.m_max));
    m_hardness.setValue(preset.m_hardness, true);
    m_opacity.setValue(
        TDoublePairProperty::Value(preset.m_opacityMin, preset.m_opacityMax));
    m_pressure.setValue(preset.m_pressure);
    m_modifierSize.setValue(preset.m_modifierSize);
    m_modifierOpacity.setValue(preset.m_modifierOpacity);
    m_modifierEraser.setValue(preset.m_modifierEraser);
    m_modifierLockAlpha.setValue(preset.m_modifierLockAlpha);
    m_assistants.setValue(preset.m_assistants);
    
    // Style snapshot restoration.
    // When Selective Preset mode is ON, this entire block is skipped: only the
    // tool parameters above are injected. The user's current style acts as a
    // "base" that receives dynamic brush settings from any chosen preset.
    if (preset.m_styleInfoVersion >= 1 &&
        !BrushPresetBridge::isSelectivePresetMode()) {
      if (TTool::Application *app = getApplication()) {
        TPaletteHandle *paletteHandle =
            app->getPaletteController()->getCurrentLevelPalette();
        TPalette *palette = paletteHandle ? paletteHandle->getPalette() : nullptr;
        int styleIndex = app ? app->getCurrentLevelStyleIndex() : 0;
        // Style 0 is the transparent/eraser style and must never be
        // overwritten, matching the Level Palette protection rules.
        if (palette && styleIndex >= 1) {
            TColorStyle *currentStyle = palette->getStyle(styleIndex);
            TPixel32 currentColor = TPixel32::Black;
            if (currentStyle) {
              if (dynamic_cast<TTextureStyle *>(currentStyle) &&
                  currentStyle->getColorParamCount() > 0)
                currentColor = currentStyle->getColorParamValue(0);
              else
                currentColor = currentStyle->getMainColor();
            }
            
            auto commitStyle = [&](TColorStyle *newStyle) {
              if (BrushPresetBridge::isNonDestructiveMode()) {
                if (TSelection *sel =
                        app->getCurrentSelection()->getSelection())
                  if (TStyleSelection *ss =
                          dynamic_cast<TStyleSelection *>(sel))
                    ss->selectNone();

                int matchIdx =
                    BrushPresetBridge::findMatchingStyleInPalette(palette, newStyle);
                if (matchIdx >= 0) {
                  delete newStyle;
                  app->setCurrentLevelStyleIndex(matchIdx, true);
                } else {
                  int newId = palette->addStyle(newStyle);
                  if (newId >= 0) {
                    for (int p = 0; p < palette->getPageCount(); ++p) {
                      TPalette::Page *pg = palette->getPage(p);
                      if (!pg) continue;
                      for (int s = 0; s < pg->getStyleCount(); ++s) {
                        if (pg->getStyleId(s) == styleIndex) {
                          pg->addStyle(newId);
                          p = palette->getPageCount();
                          break;
                        }
                      }
                    }
                    palette->setDirtyFlag(true);
                    app->setCurrentLevelStyleIndex(newId, true);
                    paletteHandle->notifyPaletteChanged();
                  }
                }
              } else {
                TColorStyle *oldStyle = palette->getStyle(styleIndex);
                TUndo *undo = BrushPresetBridge::createStyleOverwriteUndo(
                    palette, styleIndex, oldStyle, newStyle, paletteHandle);
                palette->setStyle(styleIndex, newStyle);
                paletteHandle->notifyColorStyleChanged(false);
                TUndoManager::manager()->add(undo);
              }
            };
            
            if (preset.m_hasStyleSnapshot && preset.m_styleInfoVersion >= 3) {
              TColorStyle *newStyle = nullptr;
              
              if (preset.m_snapshotStyleTagId == 4001) {
                TFilePath mpPath(preset.m_snapshotFilePath);
                newStyle = new TMyPaintBrushStyle(mpPath);
              } else {
                newStyle = TColorStyle::create(preset.m_snapshotBrushIdName);
              }
              
              if (newStyle) {
                for (const auto &param : preset.m_snapshotParams) {
                  int idx = param.first;
                  double val = param.second;
                  if (idx >= newStyle->getParamCount()) continue;
                  TColorStyle::ParamType ptype = newStyle->getParamType(idx);
                  switch (ptype) {
                    case TColorStyle::BOOL:
                      newStyle->setParamValue(idx, (bool)(val != 0));
                      break;
                    case TColorStyle::INT:
                    case TColorStyle::ENUM:
                      newStyle->setParamValue(idx, (int)val);
                      break;
                    case TColorStyle::DOUBLE:
                      newStyle->setParamValue(idx, val);
                      break;
                    default:
                      break;
                  }
                }
                newStyle->setMainColor(currentColor);
                commitStyle(newStyle);
              }
            } else if (preset.m_hasMyPaint) {
              TFilePath myPaintPath(preset.m_myPaintPath);
              TMyPaintBrushStyle *newStyle = new TMyPaintBrushStyle(myPaintPath);
              newStyle->setMainColor(currentColor);
              commitStyle(newStyle);
            } else if (!preset.m_hasMyPaint && !preset.m_hasTexture && !preset.m_hasStyleSnapshot) {
              if (BrushPresetBridge::isNonDestructiveMode() ||
                  dynamic_cast<TMyPaintBrushStyle*>(currentStyle) ||
                  dynamic_cast<TTextureStyle*>(currentStyle)) {
                TSolidColorStyle *newStyle = new TSolidColorStyle(currentColor);
                commitStyle(newStyle);
              }
            }
          }
        }
    }
  } catch (...) {
  }
}

//------------------------------------------------------------------

void FullColorBrushTool::addPreset(QString name) {
  // Build the preset
  BrushData preset(name.toStdWString());

  preset.m_min               = m_thickness.getValue().first;
  preset.m_max               = m_thickness.getValue().second;
  preset.m_hardness          = m_hardness.getValue();
  preset.m_opacityMin        = m_opacity.getValue().first;
  preset.m_opacityMax        = m_opacity.getValue().second;
  preset.m_pressure          = m_pressure.getValue();
  preset.m_modifierSize      = m_modifierSize.getValue();
  preset.m_modifierOpacity   = m_modifierOpacity.getValue();
  preset.m_modifierEraser    = m_modifierEraser.getValue();
  preset.m_modifierLockAlpha = m_modifierLockAlpha.getValue();
  preset.m_assistants        = m_assistants.getValue();
  
  // Capture complete style snapshot using the GENERIC approach.
  // This captures ALL parameters (including MyPaint's 56 params) generically.
  preset.m_styleInfoVersion = 3;
  preset.m_hasMyPaint = false;
  preset.m_hasTexture = false;
  preset.m_hasStyleSnapshot = false;
  
  if (TTool::Application *app = getApplication()) {
    TColorStyle *style = app->getCurrentLevelStyle();
    
    if (style && !dynamic_cast<TSolidColorStyle*>(style)) {
      preset.m_hasStyleSnapshot = true;
      preset.m_snapshotStyleTagId = style->getTagId();
      preset.m_snapshotBrushIdName = style->getBrushIdName();
      
      // Extract primary file path
      if (TMyPaintBrushStyle *mpStyle = dynamic_cast<TMyPaintBrushStyle*>(style)) {
        std::wstring wpath = mpStyle->getPath().getWideString();
        preset.m_snapshotFilePath = std::string(wpath.begin(), wpath.end());
        // Legacy compatibility
        preset.m_hasMyPaint = true;
        preset.m_myPaintPath = preset.m_snapshotFilePath;
      }
      
      // GENERIC: Capture ALL numeric parameters of the style
      int paramCount = style->getParamCount();
      for (int i = 0; i < paramCount; ++i) {
        TColorStyle::ParamType ptype = style->getParamType(i);
        double numValue = 0.0;
        switch (ptype) {
          case TColorStyle::BOOL:
            numValue = style->getParamValue(TColorStyle::bool_tag(), i) ? 1.0 : 0.0;
            break;
          case TColorStyle::INT:
          case TColorStyle::ENUM:
            numValue = (double)style->getParamValue(TColorStyle::int_tag(), i);
            break;
          case TColorStyle::DOUBLE:
            numValue = style->getParamValue(TColorStyle::double_tag(), i);
            break;
          case TColorStyle::FILEPATH:
            continue; // Handled via m_snapshotFilePath
          default:
            continue;
        }
        preset.m_snapshotParams.push_back({i, numValue});
      }
    }
  }

  // Pass the preset to the manager
  m_presetsManager.addPreset(preset);

  // Reinitialize the associated preset enum
  initPresets();

  // Set the value to the specified one
  m_preset.setValue(preset.m_name);
  FullcolorBrushPreset = m_preset.getValueAsString();
}

//------------------------------------------------------------------

void FullColorBrushTool::removePreset() {
  std::wstring name(m_preset.getValue());
  if (name == CUSTOM_WSTR) return;

  m_presetsManager.removePreset(name);
  initPresets();

  // No parameter change, and set the preset value to custom
  m_preset.setValue(CUSTOM_WSTR);
  FullcolorBrushPreset = m_preset.getValueAsString();
}

//------------------------------------------------------------------

void FullColorBrushTool::loadLastBrush() {
  m_thickness.setValue(
      TIntPairProperty::Value(FullcolorBrushMinSize, FullcolorBrushMaxSize));
  m_pressure.setValue(FullcolorPressureSensitivity ? 1 : 0);
  m_opacity.setValue(
      TDoublePairProperty::Value(FullcolorMinOpacity, FullcolorMaxOpacity));
  m_hardness.setValue(FullcolorBrushHardness);
  m_modifierSize.setValue(FullcolorModifierSize);
  m_modifierOpacity.setValue(FullcolorModifierOpacity);
  m_modifierEraser.setValue(FullcolorModifierEraser ? true : false);
  m_modifierLockAlpha.setValue(FullcolorModifierLockAlpha ? true : false);
  m_assistants.setValue(FullcolorAssistants ? true : false);
}

//------------------------------------------------------------------

void FullColorBrushTool::updateCurrentStyle() {
  m_currentColor = TPixel32::Black;
  if (TTool::Application *app = getApplication()) {
    if (app->getCurrentObject()->isSpline()) {
      m_currentColor = TPixel32::Red;
    } else if (TPalette *plt = app->getCurrentPalette()->getPalette()) {
      int style               = app->getCurrentLevelStyleIndex();
      TColorStyle *colorStyle = plt->getStyle(style);
      m_currentColor          = colorStyle->getMainColor();
    }
  }

  int prevMinCursorThick = m_minCursorThick;
  int prevMaxCursorThick = m_maxCursorThick;

  m_enabledPressure = m_pressure.getValue();
  if (m_nativeRecord) {
    m_minCursorThick = m_maxCursorThick =
        int(std::ceil(m_nativeRecord->preset.size.value *
                      std::pow(2.0, m_modifierSize.getValue())));
  } else if (TMyPaintBrushStyle *brushStyle = getBrushStyle()) {
    double radiusLog = brushStyle->getBrush().getBaseValue(
                           MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC) +
                       m_modifierSize.getValue() * log(2.0);
    double radius    = exp(radiusLog);
    m_minCursorThick = m_maxCursorThick = (int)round(2.0 * radius);
  } else {
    m_minCursorThick = std::max(m_thickness.getValue().first, 1);
    m_maxCursorThick =
        std::max(m_thickness.getValue().second, m_minCursorThick);
    if (!m_enabledPressure) m_minCursorThick = m_maxCursorThick;
  }

  // if this function is called from onEnter(), the clipping rect will not be
  // set in order to update whole viewer.
  if (prevMinCursorThick == 0 && prevMaxCursorThick == 0) return;

  if (m_minCursorThick != prevMinCursorThick ||
      m_maxCursorThick != prevMaxCursorThick) {
    TRectD rect(
        m_brushPos - TPointD(m_maxCursorThick + 2, m_maxCursorThick + 2),
        m_brushPos + TPointD(m_maxCursorThick + 2, m_maxCursorThick + 2));
    invalidate(rect);
  }
}

//------------------------------------------------------------------

TMyPaintBrushStyle *FullColorBrushTool::getBrushStyle() {
  if (TTool::Application *app = getApplication())
    return dynamic_cast<TMyPaintBrushStyle *>(app->getCurrentLevelStyle());
  return 0;
}

//------------------------------------------------------------------

void FullColorBrushTool::applyClassicToonzBrushSettings(
    mypaint::Brush &mypaintBrush) {
  const double precision       = 1e-5;
  const double hardnessOpacity = 0.1;

  double minThickness = 0.5 * m_thickness.getValue().first;
  double maxThickness = 0.5 * m_thickness.getValue().second;
  double minOpacity   = 0.01 * m_opacity.getValue().first;
  double maxOpacity   = 0.01 * m_opacity.getValue().second;
  double hardness     = 0.01 * m_hardness.getValue();

  TPixelD color = PixelConverter<TPixelD>::from(m_currentColor);
  double colorH = 0.0;
  double colorS = 0.0;
  double colorV = 0.0;
  RGB2HSV(color.r, color.g, color.b, &colorH, &colorS, &colorV);

  // avoid log(0)
  if (minThickness < precision) minThickness = precision;
  if (maxThickness < precision) maxThickness = precision;

  // tune hardness opacity for better visual softness
  hardness *= hardness;
  double opacityAmplifier = 1.0 - hardnessOpacity + hardness * hardnessOpacity;
  minOpacity *= opacityAmplifier;
  maxOpacity *= opacityAmplifier;

  // reset
  mypaintBrush.fromDefaults();
  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_OPAQUE_MULTIPLY, 1.0);
  mypaintBrush.setMappingN(MYPAINT_BRUSH_SETTING_OPAQUE_MULTIPLY,
                           MYPAINT_BRUSH_INPUT_PRESSURE, 0);

  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_HARDNESS,
                            0.5 * hardness + 0.5);
  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_COLOR_H, colorH / 360.0);
  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_COLOR_S, colorS);
  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_COLOR_V, colorV);
  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,
                            5.0 + hardness * 10.0);
  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_DABS_PER_BASIC_RADIUS, 0.0);
  mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_DABS_PER_SECOND, 0.0);
  // a tip image is stamped every 20% of its size, as Photoshop does by default
  if (m_tipImage)
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_DABS_PER_ACTUAL_RADIUS,
                              2.5);

  // thickness may be dynamic
  if (minThickness + precision >= maxThickness) {
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
                              log(maxThickness));
    mypaintBrush.setMappingN(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
                             MYPAINT_BRUSH_INPUT_PRESSURE, 0);
  } else {
    double minThicknessLog  = log(minThickness);
    double maxThicknessLog  = log(maxThickness);
    double baseThicknessLog = 0.5 * (minThicknessLog + maxThicknessLog);
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
                              baseThicknessLog);
    mypaintBrush.setMappingN(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
                             MYPAINT_BRUSH_INPUT_PRESSURE, 2);
    mypaintBrush.setMappingPoint(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
                                 MYPAINT_BRUSH_INPUT_PRESSURE, 0, 0.0,
                                 minThicknessLog - baseThicknessLog);
    mypaintBrush.setMappingPoint(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
                                 MYPAINT_BRUSH_INPUT_PRESSURE, 1, 1.0,
                                 maxThicknessLog - baseThicknessLog);
  }

  // opacity may be dynamic
  if (minOpacity + precision >= maxOpacity) {
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_OPAQUE, maxOpacity);
    mypaintBrush.setMappingN(MYPAINT_BRUSH_SETTING_OPAQUE,
                             MYPAINT_BRUSH_INPUT_PRESSURE, 0);
  } else {
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_OPAQUE, minOpacity);
    mypaintBrush.setMappingN(MYPAINT_BRUSH_SETTING_OPAQUE,
                             MYPAINT_BRUSH_INPUT_PRESSURE, 2);
    mypaintBrush.setMappingPoint(MYPAINT_BRUSH_SETTING_OPAQUE,
                                 MYPAINT_BRUSH_INPUT_PRESSURE, 0, 0.0, 0.0);
    mypaintBrush.setMappingPoint(MYPAINT_BRUSH_SETTING_OPAQUE,
                                 MYPAINT_BRUSH_INPUT_PRESSURE, 1, 1.0,
                                 maxOpacity - minOpacity);
  }

  if (m_modifierLockAlpha.getValue()) {
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_LOCK_ALPHA, 1.0);
  }
}

void FullColorBrushTool::applyToonzBrushSettings(mypaint::Brush &mypaintBrush) {
  TMyPaintBrushStyle *mypaintStyle = getBrushStyle();

  if (mypaintStyle) {
    const double precision = 1e-5;

    double modifierSize    = m_modifierSize.getValue() * log(2.0);
    double modifierOpacity = 0.01 * m_modifierOpacity.getValue();
    bool modifierEraser    = m_modifierEraser.getValue();
    bool modifierLockAlpha = m_modifierLockAlpha.getValue();

    TPixelD color = PixelConverter<TPixelD>::from(m_currentColor);
    double colorH = 0.0;
    double colorS = 0.0;
    double colorV = 0.0;
    RGB2HSV(color.r, color.g, color.b, &colorH, &colorS, &colorV);

    mypaintBrush.fromBrush(mypaintStyle->getBrush());

    float baseSize =
        mypaintBrush.getBaseValue(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC);
    float baseOpacity = mypaintBrush.getBaseValue(MYPAINT_BRUSH_SETTING_OPAQUE);

    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
                              baseSize + modifierSize);
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_OPAQUE,
                              baseOpacity * modifierOpacity);
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_COLOR_H, colorH / 360.0);
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_COLOR_S, colorS);
    mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_COLOR_V, colorV);

    if (modifierEraser) {
      mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_ERASER, 1.0);
      mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_LOCK_ALPHA, 0.0);
    } else if (modifierLockAlpha) {
      // lock-alpha already disables eraser
      mypaintBrush.setBaseValue(MYPAINT_BRUSH_SETTING_LOCK_ALPHA, 1.0);
    }
  } else {
    applyClassicToonzBrushSettings(mypaintBrush);
  }
}

//==========================================================================================================

FullColorBrushToolNotifier::FullColorBrushToolNotifier(FullColorBrushTool *tool)
    : m_tool(tool) {}

//------------------------------------------------------------

void FullColorBrushToolNotifier::onActivate() {
  if (TTool::Application *app = m_tool->getApplication()) {
    if (TXshLevelHandle *levelHandle = app->getCurrentLevel()) {
      bool ret = connect(levelHandle, SIGNAL(xshCanvasSizeChanged()), this,
                         SLOT(onCanvasSizeChanged()));
      assert(ret);
    }
    if (TPaletteHandle *paletteHandle = app->getCurrentPalette()) {
      bool ret = connect(paletteHandle, SIGNAL(colorStyleChanged(bool)), this,
                         SLOT(onColorStyleChanged()));
      ret = ret && connect(paletteHandle, SIGNAL(colorStyleSwitched()), this,
                           SLOT(onColorStyleChanged()));
      ret = ret && connect(paletteHandle, SIGNAL(paletteSwitched()), this,
                           SLOT(onColorStyleChanged()));
      assert(ret);
    }
  }
}

//------------------------------------------------------------

void FullColorBrushToolNotifier::onDeactivate() {
  if (TTool::Application *app = m_tool->getApplication()) {
    if (TXshLevelHandle *levelHandle = app->getCurrentLevel()) {
      bool ret = disconnect(levelHandle, SIGNAL(xshCanvasSizeChanged()), this,
                            SLOT(onCanvasSizeChanged()));
      assert(ret);
    }
    if (TPaletteHandle *paletteHandle = app->getCurrentPalette()) {
      bool ret = disconnect(paletteHandle, SIGNAL(colorStyleChanged(bool)),
                            this, SLOT(onColorStyleChanged()));
      ret = ret && disconnect(paletteHandle, SIGNAL(colorStyleSwitched()), this,
                              SLOT(onColorStyleChanged()));
      ret = ret && disconnect(paletteHandle, SIGNAL(paletteSwitched()), this,
                              SLOT(onColorStyleChanged()));
      assert(ret);
    }
  }
}

//==========================================================================================================

FullColorBrushTool fullColorPencil("T_Brush");
