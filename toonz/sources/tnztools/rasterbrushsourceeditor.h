#pragma once
#include "toonz/rasterbrushpreset.h"
#include "toonz/rasterbrushprofile.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <cmath>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <functional>
#include <algorithm>
#include <memory>

// Only active, already implemented source state is edited here. Preserved
// sensor/mask declarations remain original provenance, never new evaluators.
namespace RasterBrushSourceEditor {
using namespace RasterBrush;
struct State : std::enable_shared_from_this<State> {
  Record &record;
  QWidget *parent;
  QFormLayout *form;
  QLabel *message;
  std::function<void()> refresh;
  std::vector<std::function<void()>> readers;
  State(Record &r, QWidget *p, QFormLayout *f, std::function<void()> update)
      : record(r), parent(p), form(f), message(new QLabel(p)), refresh(std::move(update)) {
    message->setObjectName("sourceBrushEditStatus");
    message->setWordWrap(true);
    form->addRow(message);
  }
  void sync() { for (auto &read : readers) read(); }
  bool apply(const std::function<void(Preset &)> &change) {
    auto candidate = record.preset;
    change(candidate);
    const auto why = validate(candidate);
    if (!why.empty()) {
      message->setText(QObject::tr("This setting is unavailable with the other selected source controls: %1")
                           .arg(QString::fromStdString(why)));
      sync();
      return false;
    }
    record.preset = std::move(candidate);
    if (record.preset.sourceGeneratedV1)
      record.sourceGeneratedV1 = record.preset.sourceGeneratedV1;
    record.referenceVerified = false;
    record.provenance["sourceReferenceVerified"] = false;
    record.provenance["sourceSemanticallySupported"] = false;
    record.provenance["sourceParityQualified"] = false;
    message->clear();
    sync();
    refresh();
    return true;
  }
  void note(const QString &text) {
    auto label = new QLabel(text, parent);
    label->setWordWrap(true);
    form->addRow(label);
  }
  QDoubleSpinBox *number(const char *id, const QString &label, double lo, double hi,
      std::function<double(const Preset &)> get, std::function<void(Preset &, double)> set,
      bool enabled = true) {
    auto box = new QDoubleSpinBox(parent);
    box->setObjectName(id); box->setDecimals(15); box->setRange(lo, hi);
    box->setSingleStep(.01); box->setValue(get(record.preset)); box->setEnabled(enabled);
    form->addRow(label, box);
    readers.emplace_back([box = QPointer<QDoubleSpinBox>(box), r = &record, get] {
      if (box) { QSignalBlocker blocked(box.data()); box->setValue(get(r->preset)); }
    });
    auto self = shared_from_this();
    QObject::connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), parent,
        [self, set](double value) { self->apply([&](Preset &p) { set(p, value); }); });
    return box;
  }
  QComboBox *choice(const char *id, const QString &label, const QStringList &items,
      std::function<int(const Preset &)> get, std::function<void(Preset &, int)> set) {
    auto box = new QComboBox(parent); box->setObjectName(id); box->addItems(items);
    box->setCurrentIndex(get(record.preset)); form->addRow(label, box);
    readers.emplace_back([box = QPointer<QComboBox>(box), r = &record, get] {
      if (box) { QSignalBlocker blocked(box.data()); box->setCurrentIndex(get(r->preset)); }
    });
    auto self = shared_from_this();
    QObject::connect(box, qOverload<int>(&QComboBox::currentIndexChanged), parent,
        [self, set](int value) { self->apply([&](Preset &p) { set(p, value); }); });
    return box;
  }
  void flag(const char *id, const QString &label, std::function<bool(const Preset &)> get,
            std::function<void(Preset &, bool)> set) {
    auto box = new QCheckBox(parent); box->setObjectName(id); box->setChecked(get(record.preset));
    form->addRow(label, box);
    readers.emplace_back([box = QPointer<QCheckBox>(box), r = &record, get] {
      if (box) { QSignalBlocker blocked(box.data()); box->setChecked(get(r->preset)); }
    });
    auto self = shared_from_this();
    QObject::connect(box, &QCheckBox::toggled, parent,
        [self, set](bool value) { self->apply([&](Preset &p) { set(p, value); }); });
  }
  static Mapping identityPressure() {
    Mapping mapping; mapping.sensor = Sensor::Pressure; return mapping;
  }
  static bool knots(const QString &text, std::vector<Knot> &result) {
    result.clear();
    if (text.size() > 65536) return false;
    for (const auto &point : text.split(';', Qt::SkipEmptyParts)) {
      const auto pair = point.trimmed().split(',');
      if (pair.size() != 2) return false;
      bool xok = false, yok = false;
      const double x = pair[0].trimmed().toDouble(&xok), y = pair[1].trimmed().toDouble(&yok);
      if (!xok || !yok || !std::isfinite(x) || !std::isfinite(y)) return false;
      result.push_back({x, y});
      if (result.size() > 1024) return false;
    }
    return !result.empty();
  }
  QWidget *curve(const char *id, const QString &label,
      std::function<std::vector<Knot>(const Preset &)> get,
      std::function<void(Preset &, const std::vector<Knot> &)> set) {
    auto row = new QWidget(parent); auto layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto edit = new QLineEdit(row); edit->setObjectName(id); edit->setMaxLength(65536);
    auto button = new QPushButton(QObject::tr("Apply curve"), row);
    button->setObjectName(QString(id) + "Apply"); layout->addWidget(edit); layout->addWidget(button);
    form->addRow(label, row);
    auto read = [edit = QPointer<QLineEdit>(edit), r = &record, get] {
      if (!edit) return;
      QStringList points;
      for (const auto &point : get(r->preset))
        points << QString::number(point.x, 'g', 17) + "," + QString::number(point.y, 'g', 17);
      edit->setText(points.join(';') + ';');
    };
    readers.emplace_back(read); read();
    auto self = shared_from_this();
    QObject::connect(button, &QPushButton::clicked, parent, [self, edit, set] {
      std::vector<Knot> points;
      if (!knots(edit->text(), points)) {
        self->message->setText(QObject::tr("Use up to 1024 input,output pairs separated by semicolons."));
        return;
      }
      self->apply([&](Preset &p) { set(p, points); });
    });
    return row;
  }
  void pressure(const char *id, const QString &label, Dynamic Preset::*member,
                bool arbitraryCurve) {
    auto input = choice(id, label, {QObject::tr("Fixed"), QObject::tr("Pressure")},
        [member](const Preset &p) { return (p.*member).mappings.empty() ? 0 : 1; },
        [member](Preset &p, int mode) {
          auto &control = p.*member; control.minimum = 0;
          control.mappings = mode ? std::vector<Mapping>{identityPressure()} : std::vector<Mapping>{};
        });
    if (member == &Preset::flow)
      readers.emplace_back([input = QPointer<QComboBox>(input), r = &record] {
        if (input) input->setEnabled(r->preset.wash);
      });
    if (arbitraryCurve) {
      auto row = curve((QString(id) + "Curve").toLatin1().constData(), label + QObject::tr(" curve"),
          [member](const Preset &p) { return (p.*member).mappings.empty()
              ? std::vector<Knot>{{0, 0}, {1, 1}} : (p.*member).mappings.front().curve; },
          [member](Preset &p, const std::vector<Knot> &points) {
            auto mapping = identityPressure(); mapping.curve = points;
            mapping.interpolation = points.size() > 2 ? Interpolation::NaturalCubic : Interpolation::Linear;
            (p.*member).minimum = 0; (p.*member).mappings = {mapping};
          });
      readers.emplace_back([row = QPointer<QWidget>(row), r = &record, member] {
        if (row) row->setEnabled(!(r->preset.*member).mappings.empty() &&
                                 (member != &Preset::flow || r->preset.wash));
      });
    }
  }
};
inline void add(Record &record, QWidget *parent, QFormLayout *form, std::function<void()> refresh) {
  const auto descriptor = profileDescriptorV1(record.preset.profile);
  if (!descriptor.isSourceCandidate()) return;
  auto ui = std::make_shared<State>(record, parent, form, std::move(refresh));
  ui->note(QObject::tr("These controls edit the current source candidate only. Unsupported combinations are rejected. Preserved original sensor and mask declarations remain metadata. Convert a copy to Native to use native controls."));
  const bool generated = descriptor.tip == TipFamilyV1::GeneratedMask;
  const bool wet = descriptor.family == ProfileFamilyV1::SourceWet;
  const bool hard = descriptor.id == ProfileIdV1::HardPixel;
  if (generated) {
    ui->choice("sourceBrushGenerator", QObject::tr("Source generator"),
        {QObject::tr("Soft (new selection starts with an identity curve)"), QObject::tr("Gaussian")},
        [](const Preset &p) { return p.sourceGeneratedV1->generator == SourceGeneratedGenerator::Soft ? 0 : 1; },
        [](Preset &p, int value) {
          auto &d = *p.sourceGeneratedV1;
          d.generator = value ? SourceGeneratedGenerator::Gaussian : SourceGeneratedGenerator::Soft;
          d.softnessCurve = value ? std::vector<Knot>{} : std::vector<Knot>{{0, 0}, {1, 1}};
        });
    ui->choice("sourceBrushShape", QObject::tr("Source shape"), {QObject::tr("Circle"), QObject::tr("Rectangle")},
        [](const Preset &p) { return int(p.sourceGeneratedV1->shape); },
        [](Preset &p, int value) {
          p.sourceGeneratedV1->shape = SourceGeneratedShape(value);
          p.tip.shape = value ? Shape::Square : Shape::Round;
        });
    ui->choice("sourceBrushGeneratedSize", QObject::tr("Source diameter"), {"16", "64"},
        [](const Preset &p) { return p.size.value == 16 ? 0 : 1; },
        [](Preset &p, int value) { p.size.value = p.sourceGeneratedV1->diameter = value ? 64 : 16; });
    auto horizontalFade = ui->number("sourceBrushHorizontalFade", QObject::tr("Source horizontal fade"), 0, 1,
        [](const Preset &p) { return p.sourceGeneratedV1->horizontalFade; },
        [](Preset &p, double value) { p.sourceGeneratedV1->horizontalFade = value; });
    auto verticalFade = ui->number("sourceBrushVerticalFade", QObject::tr("Source vertical fade"), 0, 1,
        [](const Preset &p) { return p.sourceGeneratedV1->verticalFade; },
        [](Preset &p, double value) { p.sourceGeneratedV1->verticalFade = value; });
    ui->readers.emplace_back([horizontalFade = QPointer<QDoubleSpinBox>(horizontalFade),
        verticalFade = QPointer<QDoubleSpinBox>(verticalFade), r = &record] {
      const bool active = r->preset.sourceGeneratedV1->generator == SourceGeneratedGenerator::Gaussian;
      if (horizontalFade) horizontalFade->setEnabled(active);
      if (verticalFade) verticalFade->setEnabled(active);
    });
    auto softness = ui->curve("sourceBrushSoftnessCurve", QObject::tr("Soft source curve"),
        [](const Preset &p) { return p.sourceGeneratedV1->softnessCurve; },
        [](Preset &p, const std::vector<Knot> &points) {
          p.sourceGeneratedV1->generator = SourceGeneratedGenerator::Soft;
          p.sourceGeneratedV1->softnessCurve = points;
        });
    ui->readers.emplace_back([row = QPointer<QWidget>(softness), r = &record] {
      if (row) row->setEnabled(r->preset.sourceGeneratedV1->generator == SourceGeneratedGenerator::Soft);
    });
    ui->note(QObject::tr("Soft uses its cubic curve; its fade declarations are preserved and do not change that response. Gaussian uses the independent fades. AA is fixed on, ratio is 1 and rotation is 0 in this candidate."));
  } else {
    const bool bitmap = descriptor.family == ProfileFamilyV1::SourceBitmap;
    const int extent = bitmap ? std::max(record.preset.tip.images.front()->width,
                                         record.preset.tip.images.front()->height) : 0;
    auto size = ui->number("sourceBrushSize", QObject::tr("Source diameter"), bitmap ? extent : 1,
        bitmap ? 2 * extent : 4096,
        [](const Preset &p) { return p.size.value; }, [](Preset &p, double value) { p.size.value = value; },
        !descriptor.usesDeliveredTimer() && descriptor.stochastic != StochasticProcessV1::SourceScatter);
    size->setSingleStep(bitmap ? .01 : 1);
    ui->number("sourceBrushAspect", QObject::tr("Source aspect"), .01, 1,
        [](const Preset &p) { return p.aspect.value; }, [](Preset &p, double value) { p.aspect.value = value; }, hard);
    auto angle = ui->number("sourceBrushAngle", QObject::tr("Source angle (degrees, modulo 360)"), -36000, 36000,
        [](const Preset &p) { return p.angle.value; }, [](Preset &p, double value) { p.angle.value = value; },
        hard || descriptor.family == ProfileFamilyV1::SourceBitmap);
    ui->readers.emplace_back([angle = QPointer<QDoubleSpinBox>(angle), r = &record, hard, bitmap] {
      if (angle) angle->setEnabled((hard || bitmap) && r->preset.angle.mappings.empty());
    });
  }
  if (descriptor.tip == TipFamilyV1::DefaultCircle || descriptor.tip == TipFamilyV1::DefaultRectangle) {
    ui->number("sourceBrushDefaultFade", QObject::tr("Default source fade"), 0, 1,
        [](const Preset &p) { return p.tip.hardness; }, [](Preset &p, double value) { p.tip.hardness = value; });
    ui->flag("sourceBrushDefaultAA", QObject::tr("Default source antialiasing"),
        [](const Preset &p) { return p.tip.antialiasEdges; }, [](Preset &p, bool value) { p.tip.antialiasEdges = value; });
  }
  const bool transfer = generated || hard || descriptor.tip == TipFamilyV1::DefaultCircle ||
      descriptor.tip == TipFamilyV1::DefaultRectangle || wet;
  if (descriptor.family == ProfileFamilyV1::SourcePixel)
    ui->note(QObject::tr("Source Flow affects Wash. Its saved controls are inactive in source build-up; zero Flow still paints in that mode."));
  ui->number("sourceBrushSpacing", QObject::tr("Source spacing"), generated ? .25 : .01, 5,
      [](const Preset &p) { return p.spacing; }, [](Preset &p, double value) { p.spacing = value; },
      !descriptor.usesDeliveredTimer() && descriptor.stochastic != StochasticProcessV1::SourceScatter);
  ui->number("sourceBrushOpacity", QObject::tr("Source opacity"), 0, 1,
      [](const Preset &p) { return p.opacity.value; }, [](Preset &p, double value) { p.opacity.value = value; },
      transfer && !descriptor.usesDeliveredTimer());
  auto flow = ui->number("sourceBrushFlow", QObject::tr("Source flow (Wash only)"), 0, 1,
      [](const Preset &p) { return p.flow.value; }, [](Preset &p, double value) { p.flow.value = value; });
  ui->readers.emplace_back([flow = QPointer<QDoubleSpinBox>(flow), r = &record, transfer, wet] {
    if (flow) flow->setEnabled(transfer && !wet && r->preset.wash);
  });
  if (!wet && descriptor.family == ProfileFamilyV1::SourcePixel &&
      !descriptor.usesDeliveredTimer() && descriptor.stochastic != StochasticProcessV1::SourceScatter) {
    ui->flag("sourceBrushWash", QObject::tr("Source Wash"), [](const Preset &p) { return p.wash; },
        [](Preset &p, bool value) { p.wash = value; });
    ui->choice("sourceBrushBlend", QObject::tr("Source blend"),
        {QObject::tr("Normal"), QObject::tr("Multiply"), QObject::tr("Screen"), QObject::tr("Overlay")},
        [](const Preset &p) { return int(p.blend); }, [](Preset &p, int value) { p.blend = Blend(value); });
  }
  if (hard || generated) {
    ui->pressure("sourceBrushOpacityInput", QObject::tr("Source opacity input"), &Preset::opacity, true);
    ui->pressure("sourceBrushFlowInput", QObject::tr("Source flow input"), &Preset::flow, true);
    if (hard) {
      ui->pressure("sourceBrushSizeInput", QObject::tr("Source size input"), &Preset::size, true);
      ui->choice("sourceBrushAngleInput", QObject::tr("Source angle input"),
          {QObject::tr("Fixed"), QObject::tr("Barrel rotation"), QObject::tr("Tilt X"), QObject::tr("Tilt Y")},
          [](const Preset &p) {
            if (p.angle.mappings.empty()) return 0;
            const auto sensor = p.angle.mappings.front().sensor;
            return sensor == Sensor::Rotation ? 1 : sensor == Sensor::TiltX ? 2 : 3;
          }, [](Preset &p, int value) {
            p.angle.minimum = 0;
            if (!value) { p.angle.value = 0; p.angle.mappings.clear(); }
            else {
              auto mapping = State::identityPressure();
              mapping.sensor = value == 1 ? Sensor::Rotation : value == 2 ? Sensor::TiltX : Sensor::TiltY;
              p.angle.value = 360; p.angle.mappings = {mapping};
            }
          });
    }
  }
  if (record.preset.sourceScatterV1) {
    ui->choice("sourceBrushScatterAxes", QObject::tr("Source scatter axes (amount 1, diameter 8, spacing 4)"),
        {QObject::tr("X"), QObject::tr("Y"), QObject::tr("X and Y")},
        [](const Preset &p) { return p.sourceScatterV1->axisX ? (p.sourceScatterV1->axisY ? 2 : 0) : 1; },
        [](Preset &p, int value) {
          p.sourceScatterV1->axisX = value != 1; p.sourceScatterV1->axisY = value != 0;
        });
  }
  if (descriptor.family == ProfileFamilyV1::SourceBitmap &&
      descriptor.stochastic != StochasticProcessV1::SourceGihProposalCache &&
      record.preset.tip.images.size() == 1 && !record.preset.tip.images.front()->color) {
    ui->choice("sourceBrushMirrorAxes", QObject::tr("Source identity-pressure Mirror"),
        {QObject::tr("Off"), QObject::tr("Horizontal"), QObject::tr("Vertical"), QObject::tr("Both")},
        [](const Preset &p) { return !p.sourceMirror.enabled ? 0 :
            (p.sourceMirror.horizontal ? 1 : 0) + (p.sourceMirror.vertical ? 2 : 0); },
        [](Preset &p, int value) {
          p.sourceMirror = {};
          if (value) {
            p.sourceMirror.enabled = true; p.sourceMirror.horizontal = value & 1;
            p.sourceMirror.vertical = value & 2; p.sourceMirror.control.mappings = {State::identityPressure()};
          }
        });
  }
  if (hard) {
    ui->choice("sourceBrushMaskMode", QObject::tr("Source generated secondary mask"),
        {QObject::tr("Off"), QObject::tr("Multiply"), QObject::tr("Subtract")},
        [](const Preset &p) { return p.sourceMasked.enabled ? int(p.sourceMasked.mode) + 1 : 0; },
        [](Preset &p, int value) {
          p.sourceMasked.enabled = value != 0;
          if (value) p.sourceMasked.mode = MaskMode(value - 1);
        });
    ui->flag("sourceBrushMaskMaster", QObject::tr("Mask uses primary size"),
        [](const Preset &p) { return p.sourceMasked.useMasterSize; },
        [](Preset &p, bool value) { p.sourceMasked.useMasterSize = value; });
    auto maskDiameter = ui->number("sourceBrushMaskDiameter", QObject::tr("Independent mask diameter"), 1, 4096,
        [](const Preset &p) { return p.sourceMasked.diameter; },
        [](Preset &p, double value) { p.sourceMasked.diameter = value; });
    maskDiameter->setSingleStep(1);
    auto maskSize = ui->number("sourceBrushMaskSize", QObject::tr("Mask size relative to primary"), 1.0 / 4096, 16,
        [](const Preset &p) { return p.sourceMasked.masterSizeCoeff; },
        [](Preset &p, double value) { p.sourceMasked.masterSizeCoeff = value; });
    maskSize->setDecimals(17);
    maskSize->setSingleStep(1 / record.preset.size.value);
    ui->number("sourceBrushMaskAspect", QObject::tr("Mask aspect"), .01, 1,
        [](const Preset &p) { return p.sourceMasked.aspect; },
        [](Preset &p, double value) { p.sourceMasked.aspect = value; });
    ui->number("sourceBrushMaskSpacing", QObject::tr("Mask spacing"), .01, 5,
        [](const Preset &p) { return p.sourceMasked.spacing; },
        [](Preset &p, double value) { p.sourceMasked.spacing = value; });
    ui->number("sourceBrushMaskOpacity", QObject::tr("Mask opacity"), 0, 1,
        [](const Preset &p) { return p.sourceMasked.opacity; },
        [](Preset &p, double value) { p.sourceMasked.opacity = value; });
    ui->number("sourceBrushMaskFlow", QObject::tr("Mask flow"), 0, 1,
        [](const Preset &p) { return p.sourceMasked.flow; },
        [](Preset &p, double value) { p.sourceMasked.flow = value; });
    ui->note(QObject::tr("This is the implemented generated secondary field in Wash mode. It does not activate preserved secondary image resources or unimplemented source mask sensors."));
  }
  if (hard || wet) {
    ui->choice("sourceBrushFixedHsvChannel", QObject::tr("Fixed source HSV channel"),
        {QObject::tr("Off"), QObject::tr("Hue"), QObject::tr("Saturation"), QObject::tr("Value")},
        [](const Preset &p) { return int(p.sourceHsvChannel); },
        [](Preset &p, int value) {
          p.sourceHsvChannel = SourceHsvChannel(value); p.sourceHsvValue = .5;
          if (value) p.sourceHsvV2 = {};
        });
    auto fixedHsvValue = ui->number("sourceBrushFixedHsvValue", QObject::tr("Fixed source HSV value"), 0, 1,
        [](const Preset &p) { return p.sourceHsvValue; },
        [](Preset &p, double value) { p.sourceHsvValue = value; });
    ui->readers.emplace_back([value = QPointer<QDoubleSpinBox>(fixedHsvValue), r = &record] {
      if (value) value->setEnabled(r->preset.sourceHsvChannel != SourceHsvChannel::Off);
    });
    const QStringList names{QObject::tr("Hue"), QObject::tr("Saturation"), QObject::tr("Value")};
    for (int channel = 0; channel < 3; ++channel) {
      const auto id = QString("sourceBrushHsv%1").arg(channel);
      ui->choice((id + "Input").toLatin1().constData(), names[channel] + QObject::tr(" source input"),
          {QObject::tr("Off"), QObject::tr("Fixed"), QObject::tr("Identity pressure")},
          [channel](const Preset &p) { const auto &c = p.sourceHsvV2[channel];
            return !c.enabled ? 0 : c.control.mappings.empty() ? 1 : 2; },
          [channel](Preset &p, int mode) {
            auto &c = p.sourceHsvV2[channel]; c = {};
            if (mode) {
              p.sourceHsvChannel = SourceHsvChannel::Off; p.sourceHsvValue = .5;
              c.enabled = true; c.control = Dynamic{mode == 2 ? 1. : .5, 0, {}};
              if (mode == 2) c.control.mappings = {State::identityPressure()};
            }
          });
      auto value = ui->number((id + "Value").toLatin1().constData(), names[channel] + QObject::tr(" fixed source value"), 0, 1,
          [channel](const Preset &p) { return p.sourceHsvV2[channel].control.value; },
          [channel](Preset &p, double v) { p.sourceHsvV2[channel].control.value = v; });
      ui->readers.emplace_back([value = QPointer<QDoubleSpinBox>(value), r = &record, channel] {
        if (value) value->setEnabled(r->preset.sourceHsvV2[channel].enabled &&
                                    r->preset.sourceHsvV2[channel].control.mappings.empty());
      });
    }
  }
  if (wet) {
    ui->note(QObject::tr("The source wet model is fixed by the selected profile. Flow is inactive in its current build-up path; preserved declarations do not establish additional behavior."));
    ui->number("sourceBrushPickup", QObject::tr("Source smudge strength"), 0, 1,
        [](const Preset &p) { return p.pickup; }, [](Preset &p, double v) { p.pickup = v; });
    ui->number("sourceBrushColorRate", QObject::tr("Source color rate"), 0, 1,
        [](const Preset &p) { return p.colorRate; }, [](Preset &p, double v) { p.colorRate = v; });
    const bool dulling = record.preset.wet == Wet::Dulling;
    ui->number("sourceBrushRadius", QObject::tr("Source Dulling radius"), 0,
        descriptor.id == ProfileIdV1::LegacyDulling ? 1 : 3,
        [](const Preset &p) { return p.smudgeRadius; }, [](Preset &p, double v) { p.smudgeRadius = v; }, dulling);
    ui->flag("sourceBrushLengthEnabled", QObject::tr("Source smudge length enabled"),
        [](const Preset &p) { return p.sourceSmudgeLengthEnabled; },
        [](Preset &p, bool v) { p.sourceSmudgeLengthEnabled = v; });
    ui->flag("sourceBrushColorEnabled", QObject::tr("Source color rate enabled"),
        [](const Preset &p) { return p.sourceColorRateEnabled; },
        [](Preset &p, bool v) { p.sourceColorRateEnabled = v; });
    ui->flag("sourceBrushSmearAlpha", QObject::tr("Source smear alpha"),
        [](const Preset &p) { return p.sourceSmearAlpha; },
        [](Preset &p, bool v) { p.sourceSmearAlpha = v; });
    ui->pressure("sourceBrushPickupInput", QObject::tr("Source strength input"), &Preset::pickupControl, false);
    ui->pressure("sourceBrushColorInput", QObject::tr("Source color rate input"), &Preset::colorRateControl, false);
    ui->pressure("sourceBrushOpacityInput", QObject::tr("Source opacity input"), &Preset::opacity, false);
  }
  if (record.preset.texture.image) {
    ui->choice("sourceBrushTextureMode", QObject::tr("Source texture mode"), {QObject::tr("Multiply"), QObject::tr("Subtract")},
        [](const Preset &p) { return int(p.texture.mode); }, [](Preset &p, int v) { p.texture.mode = MaskMode(v); });
    ui->number("sourceBrushTextureDepth", QObject::tr("Source texture depth"), 0, 1,
        [](const Preset &p) { return p.texture.depth; }, [](Preset &p, double v) { p.texture.depth = v; });
    ui->flag("sourceBrushTextureInvert", QObject::tr("Invert source texture"),
        [](const Preset &p) { return p.texture.invert; }, [](Preset &p, bool v) { p.texture.invert = v; });
    ui->flag("sourceBrushTextureSoft", QObject::tr("Source soft texturing"),
        [](const Preset &p) { return p.sourceTextureSoftTexturing; }, [](Preset &p, bool v) { p.sourceTextureSoftTexturing = v; });
  }
  if (descriptor.usesDeliveredTimer()) {
    ui->choice("sourceBrushAirbrushRate", QObject::tr("Source delivered airbrush rate"), {"5 Hz", "20 Hz"},
        [](const Preset &p) { return p.airbrushRate == 5 ? 0 : 1; },
        [](Preset &p, int v) { p.airbrushRate = v ? 20 : 5; });
    ui->flag("sourceBrushAirbrushIgnoreSpacing", QObject::tr("Source airbrush ignores distance spacing"),
        [](const Preset &p) { return p.airbrushIgnoreSpacing; },
        [](Preset &p, bool v) { p.airbrushIgnoreSpacing = v; });
  }
  ui->sync();
}
} // namespace RasterBrushSourceEditor
