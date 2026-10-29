#include "toonzqt/seexprcontrols.h"
#include "tseexprspectrum.h"
#include "tseexpranimcurve.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "texpression.h"
#include "tunit.h"
#include "toonz/txsheetexpr.h"

#include <QCheckBox>
#include <QAbstractItemModel>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QIntValidator>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace SeExprControls {
namespace {
QStringList arguments(const QString &source) {
  QStringList values;
  int start = 0, depth = 0;
  QChar quote;
  bool escaped = false, comment = false;
  for (int i = 0; i < source.size(); ++i) {
    const QChar c = source[i];
    if (comment) { if (c == '\n') comment = false; continue; }
    if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
    } else if (c == '#') comment = true;
    else if (c == '"' || c == '\'') quote = c;
    else if (c == '(' || c == '[' || c == '{') ++depth;
    else if (c == ')' || c == ']' || c == '}') --depth;
    else if (c == ',' && depth == 0) {
      values.push_back(source.mid(start, i - start).trimmed());
      start = i + 1;
    }
  }
  values.push_back(source.mid(start).trimmed());
  return values;
}

bool number(const QString &source, double &value) {
  static const QRegularExpression syntax(
      "^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$");
  if (!syntax.match(source.trimmed()).hasMatch()) return false;
  bool valid = false;
  value = source.trimmed().toDouble(&valid);
  return valid && std::isfinite(value);
}

bool vector(const QString &source, QJsonArray &value) {
  const QString s = source.trimmed();
  if (!s.startsWith('[') || !s.endsWith(']')) return false;
  const auto components = arguments(s.mid(1, s.size() - 2));
  if (components.size() != 3) return false;
  for (const auto &component : components) {
    double n;
    if (!number(component, n)) return false;
    value.append(n);
  }
  return true;
}

QString annotation(const QString &source, int end) {
  if (end < source.size() && source[end] == ';') ++end;
  while (end < source.size() && (source[end] == ' ' || source[end] == '\t')) ++end;
  if (end >= source.size() || source[end] != '#') return {};
  int last = source.indexOf('\n', end);
  if (last < 0) last = source.size();
  return source.mid(end + 1, last - end - 1).trimmed();
}

bool curve(const QStringList &args, bool color, QJsonArray &points) {
  if (args.size() < 7 || (args.size() - 1) % 3 != 0 || args.size() > 193)
    return false;
  double previous = -1.0;
  for (int i = 1; i < args.size(); i += 3) {
    double position, mode;
    if (!number(args[i], position) || !number(args[i + 2], mode) ||
        position < 0 || position > 1 || position <= previous ||
        mode < 0 || mode > 4 || mode != std::floor(mode)) return false;
    previous = position;
    QJsonValue value;
    if (color) {
      QJsonArray rgb;
      if (!vector(args[i + 1], rgb)) return false;
      value = rgb;
    } else {
      double scalar;
      if (!number(args[i + 1], scalar)) return false;
      value = scalar;
    }
    points.append(QJsonArray{position, value, mode});
  }
  return true;
}

QString withoutComments(const QString &source) {
  QString result = source;
  QChar quote;
  bool escaped = false, comment = false;
  for (int i = 0; i < source.size(); ++i) {
    const QChar c = source[i];
    if (comment) {
      if (c == '\n') comment = false;
      else result[i] = ' ';
    } else if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
    } else if (c == '"' || c == '\'') quote = c;
    else if (c == '#') { comment = true; result[i] = ' '; }
  }
  return result;
}

bool stringLiteral(const QString &source, QString &value) {
  const auto document = QJsonDocument::fromJson(("[" + withoutComments(source) + "]").toUtf8());
  if (!document.isArray() || document.array().size() != 1 ||
      !document.array()[0].isString()) return false;
  value = document.array()[0].toString();
  return true;
}

QStringList expressionComments(const QString &source) {
  QStringList result;
  QChar quote;
  bool escaped = false;
  for (int i = 0; i < source.size(); ++i) {
    const QChar c = source[i];
    if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
    } else if (c == '"' || c == '\'') quote = c;
    else if (c == '#') {
      const int end = source.indexOf('\n', i);
      result.append(source.mid(i, end < 0 ? -1 : end - i));
      if (end < 0) break;
      i = end;
    }
  }
  return result;
}

bool animationCurveModel(const QJsonObject &object, TSeExprAnimCurve::Curve &curve,
                         QString &error) {
  const auto fail = [&](const QString &text) { error = text; return false; };
  const auto text = [&](const QJsonObject &owner, const char *name,
                        const char *fallback, std::string &result) {
    const auto value = owner.value(name);
    if (!value.isUndefined() && !value.isString()) return false;
    result = value.toString(QString::fromLatin1(fallback)).toUtf8().toStdString();
    return true;
  };
  const auto boolean = [&](const QJsonObject &owner, const char *name, bool &result) {
    const auto value = owner.value(name);
    if (!value.isUndefined() && !value.isBool()) return false;
    result = value.toBool(false); return true;
  };
  if (!text(object, "preInfinity", "constant", curve.preInfinity) ||
      !text(object, "postInfinity", "constant", curve.postInfinity) ||
      !text(object, "link", "", curve.link) || !boolean(object, "weighted", curve.weighted))
    return fail(QObject::tr("Animation curve infinity, link or weighting metadata has the wrong type."));
  if (object.contains("keys") && !object.value("keys").isArray())
    return fail(QObject::tr("Animation curve keys must be an array."));
  const auto keys = object.value("keys").toArray();
  if (keys.size() > 64)
    return fail(QObject::tr("A native animation curve supports at most 64 keys."));
  QSet<QString> ids;
  for (const auto &entry : keys) {
    if (!entry.isObject()) return fail(QObject::tr("Each animation key must be an object."));
    const auto object = entry.toObject();
    if (object.contains("id")) {
      const auto id = object.value("id").toString();
      static const QRegularExpression identifier("^[A-Za-z_][A-Za-z0-9_]{0,47}$");
      if (!identifier.match(id).hasMatch() || ids.contains(id))
        return fail(QObject::tr("Animation key identifiers must be valid and unique."));
      ids.insert(id);
    }
    TSeExprAnimCurve::Key key;
    const auto numeric = [&](const char *name, double &result) {
      const auto value = object.value(name);
      if (value.isUndefined()) return true;
      if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
      result = value.toDouble(); return true;
    };
    if (!numeric("time", key.time) || !numeric("value", key.value) ||
        !numeric("inWeight", key.inWeight) || !numeric("outWeight", key.outWeight) ||
        !numeric("inAngle", key.inAngle) || !numeric("outAngle", key.outAngle) ||
        !text(object, "inTangent", "auto", key.inTangent) ||
        !text(object, "outTangent", "auto", key.outTangent) ||
        !boolean(object, "weightsLocked", key.weightsLocked))
      return fail(QObject::tr("Animation key values, tangents or weight-lock metadata has the wrong type."));
    curve.keys.push_back(key);
  }
  std::string message;
  TSeExprAnimCurve::PreparedCurve prepared;
  if (!prepared.prepare(curve, message))
    return fail(QString::fromUtf8(message.c_str()));
  return true;
}

QJsonObject importAnimationCurve(const QStringList &args, QString &input,
                                 QString &error) {
  const auto fail = [&](const QString &text) { error = text; return QJsonObject(); };
  if (args.size() < 5 || (args.size() - 5) % 9 != 0)
    return fail(QObject::tr("An animation curve needs its lookup, infinity modes, weighting, link and nine fields per key."));
  if ((args.size() - 5) / 9 > 64)
    return fail(QObject::tr("A native animation curve supports at most 64 keys."));
  QString before, after, link;
  double weighted;
  if (!stringLiteral(args[1], before) || !stringLiteral(args[2], after) ||
      !number(withoutComments(args[3]), weighted) || !stringLiteral(args[4], link))
    return fail(QObject::tr("Animation curve metadata must use literal infinity modes, weighting and link."));
  QJsonObject control{{"type", "animcurve"}, {"preInfinity", before},
      {"postInfinity", after}, {"weighted", weighted != 0}, {"link", link}};
  QJsonArray keys;
  const QStringList numericNames{"time", "value", "inWeight", "outWeight", "inAngle", "outAngle"};
  for (int i = 5; i < args.size(); i += 9) {
    QJsonObject key{{"id", "key_" + QString::number(keys.size())}};
    for (int n = 0; n < numericNames.size(); ++n) {
      double value;
      if (!number(withoutComments(args[i + n]), value))
        return fail(QObject::tr("Animation key values, weights and angles must be finite numeric literals."));
      key[numericNames[n]] = value;
    }
    QString inTangent, outTangent;
    double locked;
    if (!stringLiteral(args[i + 6], inTangent) || !stringLiteral(args[i + 7], outTangent) ||
        !number(withoutComments(args[i + 8]), locked))
      return fail(QObject::tr("Animation key tangents and weight locking must use literal values."));
    key["inTangent"] = inTangent; key["outTangent"] = outTangent;
    key["weightsLocked"] = locked != 0; keys.append(key);
  }
  control["keys"] = keys;
  TSeExprAnimCurve::Curve model;
  if (!animationCurveModel(control, model, error)) return {};
  input = args[0];
  if (withoutComments(input).trimmed().isEmpty())
    return fail(QObject::tr("The animation curve lookup expression is empty."));
  QStringList comments;
  for (int i = 1; i < args.size(); ++i) comments.append(expressionComments(args[i]));
  if (!comments.isEmpty()) input += '\n' + comments.join('\n') + '\n';
  return control;
}

QJsonObject controlFor(const QString &expression, const QString &comment,
                       QString &input, QString &error) {
  QJsonObject control;
  double scalar;
  QJsonArray rgb;
  const auto range = arguments(comment);
  double minimum = 0.0, maximum = 1.0;
  const bool hasRange = range.size() == 2 && number(range[0], minimum) &&
                        number(range[1], maximum) && minimum <= maximum;
  if (number(expression, scalar)) {
    control = {{"type", "scalar"}, {"value", scalar}};
    if (hasRange) {
      control["min"] = minimum;
      control["max"] = maximum;
      const bool integer = !comment.contains('.') &&
                           !comment.contains('e', Qt::CaseInsensitive);
      control["integer"] = integer;
    }
    return control;
  }
  if (vector(expression, rgb)) {
    control = {{"type", hasRange ? "vector" : "color"}, {"value", rgb}};
    if (hasRange) { control["min"] = minimum; control["max"] = maximum; }
    return control;
  }
  if (expression.startsWith('"')) {
    const auto literal = QJsonDocument::fromJson(("[" + expression + "]").toUtf8());
    if (literal.isArray() && literal.array().size() == 1 &&
        literal.array()[0].isString()) {
      control = {{"type", "string"}, {"value", literal.array()[0]}};
      const auto words = comment.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
      if (words.size() == 2) {
        control["annotationType"] = words[0];
        control["label"] = words[1];
      }
      return control;
    }
  }
  const int open = expression.indexOf('(');
  if (open < 1 || !expression.endsWith(')')) return {};
  const QString function = expression.left(open).trimmed();
  const auto args = arguments(expression.mid(open + 1, expression.size() - open - 2));
  if (function == "animCurve") return importAnimationCurve(args, input, error);
  QJsonArray points;
  if ((function == "curve" || function == "ccurve") &&
      curve(args, function == "ccurve", points)) {
    input = args[0];
    return {{"type", function == "curve" ? "curve" : "colorcurve"},
            {"points", points}};
  }
  if (function == "swatch" && args.size() >= 3 && args.size() <= 64) {
    QJsonArray finalColor;
    for (int i = 1; i < args.size(); ++i) {
      QJsonArray color;
      if (!vector(args[i], color)) return {};
      points.append(QJsonArray{double(i - 1) / (args.size() - 1), color, 0});
      finalColor = color;
    }
    points.append(QJsonArray{1.0, finalColor, 0});
    input = args[0];
    return {{"type", "colorcurve"}, {"presentation", "swatches"},
            {"swatchCount", args.size() - 1}, {"points", points}};
  }
  return {};
}

TSeExprSpectrum::Parameters spectrumParameters(const QJsonObject &parameters) {
  TSeExprSpectrum::Parameters result;
  const auto scalar = [&](const char *key, double fallback) {
    return parameters.value(key).toDouble(fallback);
  };
  result.resolution = parameters.value("resolution").toInt(result.resolution);
  result.tileSize = scalar("tileSize", result.tileSize);
  result.lengthCutoff = scalar("lengthCutoff", result.lengthCutoff);
  result.amplitude = scalar("amplitude", result.amplitude);
  result.windAngle = scalar("windAngle", result.windAngle);
  result.windSpeed = scalar("windSpeed", result.windSpeed);
  result.directionalFactorExponent = scalar("directionalFactorExponent", result.directionalFactorExponent);
  result.directionalReflectionDamping = scalar("directionalReflectionDamping", result.directionalReflectionDamping);
  result.sharpen = scalar("sharpen", result.sharpen);
  result.time = scalar("time", result.time);
  result.filterWidth = scalar("filterWidth", result.filterWidth);
  const auto flow = parameters.value("flowDirection").toArray();
  if (!flow.isEmpty() && flow.size() != 3) {
    result.flowDirection[0] = std::numeric_limits<double>::quiet_NaN();
  } else {
    for (int i = 0; i < flow.size(); ++i)
      result.flowDirection[i] = flow[i].toDouble(std::numeric_limits<double>::quiet_NaN());
  }
  return result;
}

// The spectrum accepts finite doubles rather than a fixed number of decimal
// places. Keep small cutoffs and large values editable without expanding a
// scientific value into hundreds of zeroes in the widget.
class SpectrumNumberEdit final : public QLineEdit {
public:
  explicit SpectrumNumberEdit(QWidget *parent) : QLineEdit(parent) {
    auto *validator = new QDoubleValidator(-std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max_digits10, this);
    validator->setLocale(locale());
    validator->setNotation(QDoubleValidator::ScientificNotation);
    setValidator(validator);
  }
  void setValue(double value) {
    setText(locale().toString(value, 'g', std::numeric_limits<double>::max_digits10));
  }
  double value() const { return locale().toDouble(text()); }
  void setRange(double minimum, double maximum) {
    auto *numeric = new QDoubleValidator(minimum, maximum,
        std::numeric_limits<double>::max_digits10, this);
    numeric->setLocale(locale());
    numeric->setNotation(QDoubleValidator::ScientificNotation);
    setValidator(numeric);
  }
  void setIntegerRange(int minimum, int maximum) {
    auto *integer = new QIntValidator(minimum, maximum, this);
    integer->setLocale(locale());
    setValidator(integer);
  }
};

class SpectrumView final : public QWidget {
public:
  QJsonArray points;
  std::array<double, 2> grid{{0, 0}};
  explicit SpectrumView(QWidget *parent) : QWidget(parent) {
    setMinimumSize(360, 160);
    setAccessibleName(QObject::tr("Spectrum preview"));
  }
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), palette().base());
    const QRectF plot = QRectF(rect()).adjusted(8, 8, -8, -8);
    QColor active = palette().highlight().color();
    active.setAlpha(40);
    if (std::isfinite(grid[0]) && std::isfinite(grid[1]))
      painter.fillRect(QRectF(plot.left() + grid[0] * plot.width(), plot.top(),
                             (grid[1] - grid[0]) * plot.width(), plot.height()), active);
    painter.setPen(palette().mid().color());
    painter.drawRect(plot);
    QPainterPath path;
    bool first = true;
    for (const auto &item : points) {
      const auto point = item.toArray();
      const QPointF pixel(plot.left() + point[0].toDouble() * plot.width(),
          plot.bottom() - std::clamp(point[1].toDouble(), 0.0, 1.0) * plot.height());
      if (first) { path.moveTo(pixel); first = false; }
      else path.lineTo(pixel);
    }
    painter.setPen(QPen(palette().highlight().color(), 1.5));
    painter.drawPath(path);
  }
};

class AnimationTangentDelegate final : public QStyledItemDelegate {
public:
  explicit AnimationTangentDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                        const QModelIndex &) const override {
    auto *editor = new QComboBox(parent);
    editor->addItems({"fixed", "auto", "linear", "flat", "step", "stepnext", "spline"});
    return editor;
  }
  void setEditorData(QWidget *editor, const QModelIndex &index) const override {
    auto *combo = qobject_cast<QComboBox *>(editor);
    if (!combo) return;
    const QString value = index.data(Qt::EditRole).toString();
    int selected = combo->findText(value);
    if (selected < 0) { combo->addItem(value); selected = combo->count() - 1; }
    combo->setCurrentIndex(selected);
  }
  void setModelData(QWidget *editor, QAbstractItemModel *model,
                    const QModelIndex &index) const override {
    if (auto *combo = qobject_cast<QComboBox *>(editor))
      model->setData(index, combo->currentText(), Qt::EditRole);
  }
};

class AnimationCurveView final : public QWidget {
  struct Sample { double time, value; bool valid; };
  QVector<Sample> m_samples;
  QVector<QPointF> m_keys;
  double m_first = 0, m_last = 1, m_low = 0, m_high = 1;

  static double position(double value, double low, double high) {
    if (low == high) return 0.5;
    const double range = high - low;
    const double result = std::isfinite(range) ? (value - low) / range :
        (value * 0.5 - low * 0.5) / (high * 0.5 - low * 0.5);
    return std::clamp(result, 0.0, 1.0);
  }
public:
  explicit AnimationCurveView(QWidget *parent) : QWidget(parent) {
    setObjectName("seExprAnimationPreview");
    setAccessibleName(QObject::tr("Animation Curve Preview"));
    setMinimumSize(360, 170);
    clear(QString());
  }
  void clear(const QString &error) {
    m_samples.clear(); m_keys.clear();
    setProperty("sampleCount", 0); setProperty("keyCount", 0);
    setProperty("sampleFrames", QVariantList()); setProperty("sampleValues", QVariantList());
    setProperty("sampleError", error); update();
  }
  QString setCurve(const TSeExprAnimCurve::Curve &curve) {
    TSeExprAnimCurve::PreparedCurve prepared;
    std::string message;
    if (!prepared.prepare(curve, message)) {
      const auto error = QString::fromUtf8(message.c_str()); clear(error); return error;
    }
    m_first = curve.keys.empty() ? 0 : curve.keys.front().time;
    m_last = curve.keys.empty() ? 1 : curve.keys.back().time;
    const double span = m_last - m_first;
    const double margin = span > 0 ? span * 0.1 : std::max(1.0, std::abs(m_first) * 0.1);
    if (std::isfinite(m_first - margin)) m_first -= margin;
    if (std::isfinite(m_last + margin)) m_last += margin;
    std::vector<double> times;
    times.reserve(321);
    // Uniform samples include extrapolated values. Exact keys and their
    // adjacent representable times also show step boundaries without a second
    // interpolation implementation. Native key bounds keep this below 512.
    for (int i = 0; i <= 128; ++i) {
      const double t = double(i) / 128;
      times.push_back((1 - t) * m_first + t * m_last);
    }
    m_keys.clear();
    for (const auto &key : curve.keys) {
      m_keys.append(QPointF(key.time, key.value));
      for (double time : {std::nextafter(key.time, -std::numeric_limits<double>::infinity()),
                          key.time, std::nextafter(key.time, std::numeric_limits<double>::infinity())})
        if (std::isfinite(time) && time >= m_first && time <= m_last) times.push_back(time);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    m_samples.clear();
    QVariantList frames, values;
    QString firstError;
    m_low = std::numeric_limits<double>::max(); m_high = -m_low;
    const auto includeValue = [&](double value) {
      m_low = std::min(m_low, value); m_high = std::max(m_high, value);
    };
    for (double time : times) {
      double value = 0;
      const bool valid = prepared.evaluate(time, value, message) && std::isfinite(value);
      m_samples.append({time, value, valid}); frames.append(time);
      values.append(valid ? QVariant(value) : QVariant());
      if (valid) includeValue(value);
      else if (firstError.isEmpty()) firstError = message.empty() ?
          QObject::tr("The animation curve produced a non-finite preview value.") :
          QString::fromUtf8(message.c_str());
    }
    for (const auto &key : m_keys) includeValue(key.y());
    if (m_low > m_high) { m_low = 0; m_high = 1; }
    setProperty("sampleCount", m_samples.size()); setProperty("keyCount", m_keys.size());
    setProperty("sampleFrames", frames); setProperty("sampleValues", values);
    setProperty("sampleError", firstError); update();
    return firstError;
  }
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());
    const QRectF plot = QRectF(rect()).adjusted(55, 8, -8, -27);
    painter.setPen(palette().mid().color()); painter.drawRect(plot);
    for (int i = 1; i < 4; ++i) {
      painter.drawLine(QPointF(plot.left(), plot.top() + plot.height() * i / 4),
                       QPointF(plot.right(), plot.top() + plot.height() * i / 4));
    }
    if (m_samples.isEmpty()) return;
    const auto pixel = [&](double time, double value) {
      return QPointF(plot.left() + position(time, m_first, m_last) * plot.width(),
                     plot.bottom() - position(value, m_low, m_high) * plot.height());
    };
    QPainterPath path;
    bool connected = false;
    for (const auto &sample : m_samples) {
      if (!sample.valid) { connected = false; continue; }
      const auto point = pixel(sample.time, sample.value);
      if (!connected) path.moveTo(point); else path.lineTo(point);
      connected = true;
    }
    painter.setPen(QPen(palette().highlight().color(), 1.5)); painter.drawPath(path);
    painter.setPen(palette().text().color()); painter.setBrush(palette().base());
    for (const auto &key : m_keys) painter.drawEllipse(pixel(key.x(), key.y()), 3, 3);
    painter.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width() / 2, 20),
                     Qt::AlignLeft, locale().toString(m_first, 'g', 5));
    painter.drawText(QRectF(plot.center().x(), plot.bottom() + 4, plot.width() / 2, 20),
                     Qt::AlignRight, locale().toString(m_last, 'g', 5));
    painter.drawText(QRectF(0, plot.top(), 50, 20), Qt::AlignRight, locale().toString(m_high, 'g', 5));
    painter.drawText(QRectF(0, plot.bottom() - 20, 50, 20), Qt::AlignRight, locale().toString(m_low, 'g', 5));
  }
};

QJsonObject animationKey(double time = 0, double value = 0) {
  return {{"time", time}, {"value", value}, {"inWeight", 1.0}, {"outWeight", 1.0},
      {"inAngle", 0.0}, {"outAngle", 0.0}, {"inTangent", "auto"},
      {"outTangent", "auto"}, {"weightsLocked", false}};
}

QString jsonString(const QString &text) {
  const QString json = QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact));
  return json.mid(1, json.size() - 2);
}

QString animationMetadataSource(const QJsonObject &control) {
  QStringList fields{jsonString(control.value("preInfinity").toString("constant")),
      jsonString(control.value("postInfinity").toString("constant")),
      control.value("weighted").toBool() ? "1" : "0",
      jsonString(control.value("link").toString())};
  const QStringList numbers{"time", "value", "inWeight", "outWeight", "inAngle", "outAngle"};
  const auto defaults = animationKey();
  for (const auto &entry : control.value("keys").toArray()) {
    const auto key = entry.toObject();
    for (const auto &field : numbers)
      fields.append(QString::number(key.value(field).toDouble(defaults.value(field).toDouble()), 'g', 17));
    fields.append(jsonString(key.value("inTangent").toString("auto")));
    fields.append(jsonString(key.value("outTangent").toString("auto")));
    fields.append(key.value("weightsLocked").toBool() ? "1" : "0");
  }
  return fields.join(',');
}

struct InlineAnimationCall {
  int begin = 0, firstComma = -1, end = 0;
  QStringList args;
};

bool inlineAnimationCalls(const QString &source, QVector<InlineAnimationCall> &calls,
                           QString &error) {
  QChar quote;
  bool escaped = false, comment = false;
  for (int i = 0; i < source.size(); ++i) {
    const QChar c = source[i];
    if (comment) { if (c == '\n') comment = false; continue; }
    if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
      continue;
    }
    if (c == '#') { comment = true; continue; }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c != '_' && !c.isLetter()) continue;
    const int begin = i;
    while (i + 1 < source.size() && (source[i + 1] == '_' || source[i + 1].isLetterOrNumber())) ++i;
    if (source.mid(begin, i - begin + 1) != "animCurve" ||
        (begin && (source[begin - 1] == '$' || source[begin - 1] == ':' ||
                   source[begin - 1] == '.'))) continue;
    int open = i + 1;
    while (open < source.size() && source[open].isSpace()) ++open;
    if (open >= source.size() || source[open] != '(') continue;
    InlineAnimationCall call; call.begin = begin;
    int depth = 1, brackets = 0, braces = 0, close = open + 1;
    QChar innerQuote;
    bool innerEscaped = false, innerComment = false;
    for (; close < source.size(); ++close) {
      const QChar token = source[close];
      if (innerComment) { if (token == '\n') innerComment = false; continue; }
      if (!innerQuote.isNull()) {
        if (innerEscaped) innerEscaped = false;
        else if (token == '\\') innerEscaped = true;
        else if (token == innerQuote) innerQuote = QChar();
        continue;
      }
      if (token == '#') innerComment = true;
      else if (token == '"' || token == '\'') innerQuote = token;
      else if (token == '(') ++depth;
      else if (token == ')') { if (--depth == 0) break; }
      else if (token == '[') ++brackets;
      else if (token == ']') --brackets;
      else if (token == '{') ++braces;
      else if (token == '}') --braces;
      else if (token == ',' && depth == 1 && !brackets && !braces && call.firstComma < 0)
        call.firstComma = close;
    }
    if (close >= source.size() || brackets || braces || call.firstComma < 0) {
      error = QObject::tr("The inline animation curve call is incomplete or malformed."); return false;
    }
    call.end = close;
    call.args = arguments(source.mid(open + 1, close - open - 1));
    QString lookup, message;
    if (importAnimationCurve(call.args, lookup, message).isEmpty()) {
      error = message; return false;
    }
    calls.append(call);
  }
  return true;
}
}  // namespace

bool validateAnimationCurve(const QJsonObject &control, QString *error) {
  TSeExprAnimCurve::Curve model;
  QString message;
  const bool valid = animationCurveModel(control, model, message);
  if (error) *error = message;
  return valid;
}

QJsonObject defaultAnimationCurve() {
  auto first = animationKey(0, 0), last = animationKey(1, 1);
  first["id"] = "key_0"; last["id"] = "key_1";
  return {{"type", "animcurve"}, {"preInfinity", "constant"},
      {"postInfinity", "constant"}, {"weighted", false}, {"link", ""},
      {"keys", QJsonArray{first, last}}};
}

bool editInlineAnimationCurve(QWidget *parent, QString &source, QString *error,
                              bool createIfMissing, const AnimationCurveLinkResolver &resolver) {
  if (error) error->clear();
  if (source.toUtf8().size() > 65536) {
    if (error) *error = QObject::tr("The expression exceeds the inline curve editing limit.");
    return false;
  }
  QVector<InlineAnimationCall> calls;
  QString message;
  if (!inlineAnimationCalls(source, calls, message)) {
    if (error) *error = message; return false;
  }
  if (calls.isEmpty()) {
    if (!createIfMissing) {
      if (error) *error = QObject::tr("The expression contains no inline animation curve to edit.");
      return false;
    }
    auto control = defaultAnimationCurve();
    if (!editAnimationCurve(parent, control, resolver)) return false;
    source = "animCurve($frame," + animationMetadataSource(control) + ')';
    return true;
  }
  int selected = 0;
  if (calls.size() > 1) {
    QStringList choices;
    for (int i = 0; i < calls.size(); ++i)
      choices.append(QObject::tr("Curve %1: %2").arg(i + 1).arg(calls[i].args[0].left(96)));
    bool accepted = false;
    const QString chosen = QInputDialog::getItem(parent, QObject::tr("Edit Inline Animation Curve"),
        QObject::tr("Choose a curve"), choices, 0, false, &accepted);
    if (!accepted) return false;
    selected = choices.indexOf(chosen);
    if (selected < 0) return false;
  }
  const auto call = calls[selected];
  QString lookup;
  auto control = importAnimationCurve(call.args, lookup, message);
  if (control.isEmpty()) { if (error) *error = message; return false; }
  if (!editAnimationCurve(parent, control, resolver)) return false;
  const auto comments = expressionComments(source.mid(call.firstComma + 1,
      call.end - call.firstComma - 1));
  const QString retainedComments = comments.isEmpty() ? QString() : '\n' + comments.join('\n') + '\n';
  source.replace(call.firstComma + 1, call.end - call.firstComma - 1,
      animationMetadataSource(control) + retainedComments);
  return true;
}

bool refreshAnimationCurveLink(const TSyntax::Grammar *grammar,
                               const QString &reference, QJsonObject &control,
                               QString &error) {
  error.clear();
  const auto fail = [&](const QString &message) { error = message; return false; };
  try {
    if (!grammar) return fail(QObject::tr("The native channel context is no longer available."));
    TExpression expression;
    expression.setGrammar(grammar); expression.setText(reference.toUtf8().toStdString());
    TDoubleParam *parameter = nullptr;
    bool offsets = false;
    if (!referenceCurveForSnapshot(expression, parameter, offsets) || !parameter)
      return fail(QObject::tr("Choose one literal native channel identifier for the linked curve."));
    if (offsets)
      return fail(QObject::tr("This channel has offset layers. Use channel() to preserve its effective animated value."));
    const int count = parameter->getKeyframeCount();
    if (count > 64) return fail(QObject::tr("A native animation curve supports at most 64 keys."));
    for (int i = 0; i + 1 < count; ++i) {
      const auto &key = parameter->getKeyframe(i);
      if (key.m_step > 1 || (key.m_type != TDoubleKeyframe::Constant &&
          key.m_type != TDoubleKeyframe::Linear && key.m_type != TDoubleKeyframe::SpeedInOut))
        return fail(QObject::tr("This native interpolation cannot be copied exactly. Use channel() for stepped, ease, exponential, expression, file or similar-shape segments."));
    }
    const TMeasure *measure = parameter->getMeasure();
    const TUnit *unit = measure ? measure->getCurrentUnit() : nullptr;
    const auto convert = [&](double value) { return unit ? unit->convertTo(value) : value; };
    double factor = 1.0;
    if (unit) {
      // Only the application's known measure families have reviewed affine
      // converters. Sampling a third-party converter cannot prove it linear.
      const QString name = QString::fromStdString(measure->getName());
      const QStringList known{"length", "length.x", "length.y", "length.lx", "length.ly",
          "camera.lx", "camera.ly", "level.lx", "level.ly", "canvas.lx", "canvas.ly",
          "fxLength", "angle", "scale", "percentage", "percentage2", "shear", "colorChannel",
          "zdepth", "zdepth.cam", "zdepth.handle", "zdepth.cam.handle", "dummy"};
      if (!known.contains(name))
        return fail(QObject::tr("The channel uses an unqualified unit converter. Use channel() for its exact value."));
      const double offset = convert(0), one = convert(1), two = convert(2), negative = convert(-1);
      factor = one - offset;
      const double expectedTwo = offset + 2 * factor, expectedNegative = offset - factor;
      const double scale = std::max({1.0, std::abs(offset), std::abs(one), std::abs(two), std::abs(negative)});
      const double tolerance = 32 * std::numeric_limits<double>::epsilon() * scale;
      if (!std::isfinite(offset) || !std::isfinite(factor) || !std::isfinite(two) ||
          !std::isfinite(negative) || !std::isfinite(expectedTwo) || !std::isfinite(expectedNegative) ||
          std::abs(two - expectedTwo) > tolerance || std::abs(negative - expectedNegative) > tolerance)
        return fail(QObject::tr("The channel uses a nonlinear unit conversion. Use channel() for its exact value."));
    }
    QSet<QString> ids;
    const auto oldKeys = control.value("keys").toArray();
    for (const auto &entry : oldKeys) ids.insert(entry.toObject().value("id").toString());
    for (const auto &entry : control.value("nativeKeys").toArray())
      ids.insert(entry.toObject().value("id").toString());
    const auto idFor = [&](double time, QJsonObject &key) {
      for (const auto &entry : oldKeys) {
        const auto old = entry.toObject();
        if (old.value("time").isDouble() && old.value("time").toDouble() == time) {
          key = old;
          if (!key.value("id").toString().isEmpty()) return;
          break;
        }
      }
      QString id;
      do { id = "k_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'); }
      while (ids.contains(id));
      ids.insert(id); key["id"] = id;
    };
    QJsonArray keys;
    const int outputCount = std::max(1, count);
    for (int i = 0; i < outputCount; ++i) {
      const double time = count ? parameter->getKeyframe(i).m_frame + 1.0 : 1.0;
      QJsonObject key; idFor(time, key);
      const auto defaults = animationKey();
      for (auto it = defaults.begin(); it != defaults.end(); ++it) key[it.key()] = it.value();
      key["time"] = time;
      key["value"] = convert(count ? parameter->getKeyframe(i).m_value : parameter->getDefaultValue());
      key["inTangent"] = "linear"; key["outTangent"] = "linear";
      keys.append(key);
    }
    bool weighted = false;
    const double degrees = 180.0 / std::acos(-1.0);
    for (int i = 0; i + 1 < count; ++i) {
      const auto &native = parameter->getKeyframe(i);
      auto first = keys[i].toObject(), last = keys[i + 1].toObject();
      if (native.m_type == TDoubleKeyframe::Constant) first["outTangent"] = "step";
      else if (native.m_type == TDoubleKeyframe::SpeedInOut) {
        weighted = true;
        const auto out = parameter->getSpeedOut(i), in = parameter->getSpeedIn(i + 1);
        const double outY = out.y * factor, inY = -in.y * factor;
        first["outTangent"] = "fixed"; last["inTangent"] = "fixed";
        first["outWeight"] = std::hypot(out.x, outY);
        last["inWeight"] = std::hypot(in.x, inY);
        first["outAngle"] = std::atan2(outY, out.x) * degrees;
        last["inAngle"] = std::atan2(inY, -in.x) * degrees;
      }
      keys[i] = first; keys[i + 1] = last;
    }
    QJsonObject draft = control;
    draft["type"] = "animcurve"; draft["preInfinity"] = "constant";
    draft["postInfinity"] = parameter->isCycleEnabled() ? "cycleRelative" : "constant";
    draft["weighted"] = weighted; draft["link"] = reference; draft["keys"] = keys;
    if (!validateAnimationCurve(draft, &error)) return false;
    control = draft; return true;
  } catch (const std::exception &) {
    return fail(QObject::tr("The native linked curve could not be refreshed."));
  } catch (...) {
    return fail(QObject::tr("The native linked curve could not be refreshed."));
  }
}

ImportResult importAnnotations(const QString &source, const QJsonArray &existing,
                               const QString &sourceNamespace) {
  ImportResult result{source, existing, {}};
  if (source.toUtf8().size() > 65536) {
    result.diagnostics << QObject::tr("The expression exceeds the annotation import limit.");
    return result;
  }
  struct Edit { int begin, end; QString replacement; };
  QVector<Edit> edits;
  QSet<QString> names;
  QMap<QString, int> occurrences;
  for (const auto &value : existing) names.insert(value.toObject().value("name").toString());
  QChar quote;
  bool comment = false, escaped = false;
  for (int i = 0; i < source.size();) {
    const QChar c = source[i];
    if (comment) { if (c == '\n') comment = false; ++i; continue; }
    if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
      ++i; continue;
    }
    if (c == '#') { comment = true; ++i; continue; }
    if (c == '"' || c == '\'') { quote = c; ++i; continue; }
    if (c != '$' && c != '_' && !c.isLetter()) { ++i; continue; }
    const int tokenBegin = i;
    if (source[i] == '$') ++i;
    const int nameBegin = i;
    while (i < source.size() && (source[i].isLetterOrNumber() || source[i] == '_')) ++i;
    const QString originalName = source.mid(nameBegin, i - nameBegin);
    if (originalName.isEmpty()) { ++i; continue; }
    int equal = i;
    while (equal < source.size() && source[equal].isSpace()) ++equal;
    if (equal >= source.size() || source[equal] != '=' ||
        (equal + 1 < source.size() && source[equal + 1] == '=')) continue;
    if (tokenBegin && (source[tokenBegin - 1].isLetterOrNumber() ||
                      source[tokenBegin - 1] == '_' || source[tokenBegin - 1] == '.')) continue;
    const QString key = sourceNamespace + ':' + originalName + ':' +
                        QString::number(occurrences[originalName]++);
    int begin = equal + 1;
    while (begin < source.size() && source[begin].isSpace()) ++begin;
    int end = begin, depth = 0;
    QChar innerQuote;
    bool innerEscaped = false, innerComment = false;
    for (; end < source.size(); ++end) {
      const QChar q = source[end];
      if (innerComment) { if (q == '\n') innerComment = false; continue; }
      if (!innerQuote.isNull()) {
        if (innerEscaped) innerEscaped = false;
        else if (q == '\\') innerEscaped = true;
        else if (q == innerQuote) innerQuote = QChar();
      } else if (q == '#' && depth > 0) innerComment = true;
      else if (q == '"' || q == '\'') innerQuote = q;
      else if (q == '(' || q == '[' || q == '{') ++depth;
      else if (q == ')' || q == ']' || q == '}') --depth;
      else if (depth == 0 && (q == ';' || q == '#' || q == '\n')) break;
    }
    QString input, error;
    QJsonObject control = controlFor(source.mid(begin, end - begin).trimmed(),
                                    annotation(source, end), input, error);
    if (!error.isEmpty())
      result.diagnostics << QObject::tr("Cannot import animation curve %1: %2").arg(originalName, error);
    if (!control.isEmpty()) {
      static const QRegularExpression supportedName("^[A-Za-z_][A-Za-z0-9_]*$");
      if (!supportedName.match(originalName).hasMatch()) {
        result.diagnostics << QObject::tr("The annotated variable %1 is not a supported native identifier.")
                                  .arg(originalName);
        i = std::max(i, end);
        continue;
      }
      const QString shortName = originalName.size() <= 32 ? originalName :
          originalName.left(20) + '_' + QString::fromLatin1(QCryptographicHash::hash(
              originalName.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
      QString name = "_annot_" + shortName;
      int existingIndex = -1;
      for (int n = 0; n < result.controls.size(); ++n) {
        const auto old = result.controls[n].toObject();
        if (old.value("annotationKey").toString() == key) {
          name = old.value("name").toString();
          control["id"] = old.value("id");
          existingIndex = n;
          break;
        }
      }
      if (existingIndex < 0) {
        int suffix = 1;
        const QString base = name;
        const auto sourceContains = [&](const QString &candidate) {
          return QRegularExpression("(?<![A-Za-z0-9_])\\$?" +
              QRegularExpression::escape(candidate) + "(?![A-Za-z0-9_])")
              .match(source).hasMatch();
        };
        while (names.contains(name) || sourceContains(name))
          name = base + '_' + QString::number(suffix++);
        control["id"] = name;
      }
      control["name"] = name;
      control["annotationKey"] = key;
      if (!control.contains("label")) control["label"] = originalName;
      names.insert(name);
      if (existingIndex >= 0) result.controls[existingIndex] = control;
      else result.controls.append(control);
      edits.push_back({begin, end, input.isEmpty() ? '$' + name : name + '(' + input + ')'});
    }
    i = std::max(i, end);
  }
  for (auto it = edits.crbegin(); it != edits.crend(); ++it)
    result.source.replace(it->begin, it->end - it->begin, it->replacement);
  return result;
}

QJsonObject defaultSpectrumParameters() {
  return {{"resolution", 10}, {"tileSize", 100.0}, {"lengthCutoff", 0.01},
          {"amplitude", 1.0}, {"windAngle", 0.0}, {"windSpeed", 10.0},
          {"directionalFactorExponent", 2.0}, {"directionalReflectionDamping", 0.0},
          {"flowDirection", QJsonArray{0.0, 0.0, 0.0}}, {"sharpen", 0.0},
          {"time", 0.0}, {"filterWidth", 1.0}};
}

QJsonArray spectrumPoints(const QJsonObject &parameters, int sampleCount) {
  const auto params = spectrumParameters(parameters);
  if (!TSeExprSpectrum::valid(params)) return {};
  sampleCount = std::clamp(sampleCount, 2, 64);
  QJsonArray points;
  for (int i = 0; i < sampleCount; ++i) {
    const double position = double(i) / (sampleCount - 1);
    const double energy = TSeExprSpectrum::value(params, position);
    if (!std::isfinite(energy)) return {};
    points.append(QJsonArray{position, energy, 1});
  }
  return points;
}

bool editSwatches(QWidget *parent, QJsonObject &control) {
  QDialog dialog(parent);
  dialog.setObjectName("seExprSwatchDialog");
  dialog.setWindowTitle(QObject::tr("Color Swatches"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *table = new QTableWidget(0, 2, &dialog);
  table->setObjectName("seExprSwatchTable");
  table->setHorizontalHeaderLabels({QObject::tr("Index"), QObject::tr("Color")});
  table->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(table);
  const auto append = [&](const QJsonArray &rgb) {
    const QColor color = QColor::fromRgbF(std::clamp(rgb[0].toDouble(), 0.0, 1.0),
        std::clamp(rgb[1].toDouble(), 0.0, 1.0), std::clamp(rgb[2].toDouble(), 0.0, 1.0));
    const int row = table->rowCount();
    table->insertRow(row);
    auto *index = new QTableWidgetItem(QString::number(row));
    index->setFlags(index->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 0, index);
    // Retain full numeric RGB, including HDR and negative channels. The
    // swatch background is a display preview, never the stored color value.
    auto *item = new QTableWidgetItem(QString::fromUtf8(
        QJsonDocument(rgb).toJson(QJsonDocument::Compact)));
    item->setData(Qt::UserRole, color);
    item->setBackground(color);
    table->setItem(row, 1, item);
  };
  const auto sourcePoints = control.value("points").toArray();
  const int storedCount = control.value("swatchCount").toInt(sourcePoints.size());
  for (int i = 0; i < std::min({storedCount, int(sourcePoints.size()), 63}); ++i) {
    const auto value = sourcePoints[i];
    const auto point = value.toArray();
    const auto rgb = point.size() > 1 ? point[1].toArray() : QJsonArray();
    if (rgb.size() == 3) append(rgb);
  }
  if (!table->rowCount()) {
    append(QJsonArray{0., 0., 0.}); append(QJsonArray{1., 1., 1.});
  }
  auto *add = new QPushButton(QObject::tr("Add Color"), &dialog);
  auto *remove = new QPushButton(QObject::tr("Remove Selected"), &dialog);
  add->setObjectName("seExprSwatchAdd");
  remove->setObjectName("seExprSwatchRemove");
  layout->addWidget(add); layout->addWidget(remove);
  auto *order = new QHBoxLayout;
  auto *up = new QPushButton(QObject::tr("Move Color Up"), &dialog);
  auto *down = new QPushButton(QObject::tr("Move Color Down"), &dialog);
  up->setObjectName("seExprSwatchUp"); down->setObjectName("seExprSwatchDown");
  order->addWidget(up); order->addWidget(down); layout->addLayout(order);
  const auto move = [&](int direction) {
    const int from = table->currentRow(), to = from + direction;
    if (from < 0 || to < 0 || to >= table->rowCount()) return;
    auto *first = table->takeItem(from, 1);
    auto *second = table->takeItem(to, 1);
    table->setItem(from, 1, second); table->setItem(to, 1, first);
    table->setCurrentCell(to, 1);
  };
  QObject::connect(up, &QPushButton::clicked, &dialog, [move]() { move(-1); });
  QObject::connect(down, &QPushButton::clicked, &dialog, [move]() { move(1); });
  QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
    if (table->rowCount() < 63) append(QJsonArray{1., 1., 1.});
  });
  QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
    if (table->rowCount() > 1 && table->currentRow() >= 0) {
      table->removeRow(table->currentRow());
      for (int row = 0; row < table->rowCount(); ++row)
        table->item(row, 0)->setText(QString::number(row));
    }
  });
  QObject::connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int row, int column) {
    if (column != 1) return;
    auto *item = table->item(row, column);
    const QColor color = QColorDialog::getColor(item->data(Qt::UserRole).value<QColor>(),
        &dialog, QObject::tr("Choose Swatch Color"));
    if (!color.isValid()) return;
    item->setData(Qt::UserRole, color); item->setBackground(color);
    item->setText(QString::fromUtf8(QJsonDocument(
        QJsonArray{color.redF(), color.greenF(), color.blueF()})
        .toJson(QJsonDocument::Compact)));
  });
  auto *error = new QLabel(&dialog);
  error->setObjectName("seExprSwatchError");
  error->setWordWrap(true); layout->addWidget(error);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                     &dialog);
  buttons->setObjectName("seExprSwatchButtons");
  layout->addWidget(buttons);
  bool accepted = false;
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    QJsonArray points;
    QJsonArray finalColor;
    for (int row = 0; row < table->rowCount(); ++row) {
      QJsonArray rgb;
      const QString colorText = table->item(row, 1)->text().trimmed();
      bool colorValid = vector(colorText, rgb);
      if (!colorValid) {
        const QColor color(colorText);
        if (color.isValid()) {
          rgb = QJsonArray{color.redF(), color.greenF(), color.blueF()};
          colorValid = true;
        }
      }
      if (!colorValid) {
        table->setCurrentCell(row, 1);
        error->setText(QObject::tr("Use a finite RGB color or a valid color name."));
        return;
      }
      points.append(QJsonArray{double(row) / table->rowCount(), rgb, 0});
      finalColor = rgb;
    }
    points.append(QJsonArray{1.0, finalColor, 0});
    control["type"] = "colorcurve"; control["presentation"] = "swatches";
    control["swatchCount"] = table->rowCount();
    control["points"] = points;
    accepted = true; dialog.accept();
  });
  dialog.exec();
  return accepted;
}

bool editSpectrum(QWidget *parent, QJsonObject &control) {
  QDialog dialog(parent);
  dialog.setObjectName("seExprSpectrumDialog");
  dialog.setWindowTitle(QObject::tr("Deep Water Spectrum"));
  auto *layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(QObject::tr("Edit the spectrum used by this curve control."), &dialog));
  auto *form = new QFormLayout;
  layout->addLayout(form);
  QJsonObject params = control.value("spectrum").toObject();
  if (params.isEmpty()) params = defaultSpectrumParameters();
  QMap<QString, SpectrumNumberEdit *> fields;
  QSet<QString> changed;
  const QStringList keys{"resolution", "tileSize", "lengthCutoff", "amplitude",
      "windAngle", "windSpeed", "directionalFactorExponent", "directionalReflectionDamping",
      "sharpen", "time", "filterWidth"};
  const QStringList labels{QObject::tr("Resolution Exponent"), QObject::tr("Tile Size"),
      QObject::tr("Length Cutoff"), QObject::tr("Amplitude"), QObject::tr("Wind Angle"),
      QObject::tr("Wind Speed"), QObject::tr("Directional Exponent"),
      QObject::tr("Reflection Damping"), QObject::tr("Sharpen"), QObject::tr("Time"),
      QObject::tr("Filter Width")};
  for (int i = 0; i < keys.size(); ++i) {
    auto *field = new SpectrumNumberEdit(&dialog);
    field->setObjectName("seExprSpectrum_" + keys[i]);
    if (keys[i] == "resolution") field->setIntegerRange(1, 20);
    if (keys[i] == "lengthCutoff" || keys[i] == "amplitude")
      field->setRange(0, std::numeric_limits<double>::max());
    if (keys[i] == "directionalFactorExponent")
      field->setRange(std::nextafter(-3.99, 0.0), 32);
    if (keys[i] == "windSpeed" || keys[i] == "tileSize")
      field->setRange(std::numeric_limits<double>::denorm_min(),
                      std::numeric_limits<double>::max());
    field->setValue(params.value(keys[i]).toDouble(defaultSpectrumParameters().value(keys[i]).toDouble()));
    fields.insert(keys[i], field);
    form->addRow(labels[i], field);
  }
  const auto flow = params.value("flowDirection").toArray();
  for (int i = 0; i < 3; ++i) {
    auto *field = new SpectrumNumberEdit(&dialog);
    field->setObjectName("seExprSpectrum_flow" + QString::number(i));
    field->setValue(flow.size() > i ? flow[i].toDouble() : 0);
    fields.insert("flow" + QString::number(i), field);
    form->addRow(QObject::tr("Flow Component %1").arg(i + 1), field);
  }
  auto *view = new SpectrumView(&dialog);
  view->setObjectName("seExprSpectrumView");
  layout->addWidget(view);
  auto *error = new QLabel(&dialog);
  error->setObjectName("seExprSpectrumError");
  error->setWordWrap(true);
  layout->addWidget(error);
  const auto read = [&] {
    QJsonObject current = params;
    // Only replace edited values. Display/locale conversion must not rewrite
    // untouched doubles, optional fields, or unrelated source metadata.
    for (const auto &key : keys)
      if (changed.contains(key)) current[key] = fields[key]->value();
    QJsonArray currentFlow = flow;
    bool flowChanged = false;
    for (int i = 0; i < 3; ++i) {
      const QString key = "flow" + QString::number(i);
      if (!changed.contains(key)) continue;
      while (currentFlow.size() < 3) currentFlow.append(0.0);
      currentFlow[i] = fields[key]->value();
      flowChanged = true;
    }
    if (flowChanged) current["flowDirection"] = currentFlow;
    return current;
  };
  const auto update = [&] {
    for (auto *field : fields) {
      if (field->hasAcceptableInput()) continue;
      view->points = {}; view->grid = {{0, 0}}; view->update();
      error->setText(QObject::tr("Enter a valid number in the supported range."));
      return;
    }
    const auto current = read();
    view->points = spectrumPoints(current);
    const auto grid = TSeExprSpectrum::gridRange(spectrumParameters(current));
    const bool finiteGrid = std::isfinite(grid[0]) && std::isfinite(grid[1]);
    // Finite parameters can overflow intermediates in the logarithmic display
    // grid. Never pass its NaNs to QPainter or alter the spectrum's exact math.
    view->grid = finiteGrid ? grid : std::array<double, 2>{{0, 0}};
    if (view->points.isEmpty())
      error->setText(QObject::tr("These values do not produce a finite spectrum preview."));
    else if (!finiteGrid)
      error->setText(QObject::tr("The spectrum grid cannot be displayed for these values."));
    else
      error->clear();
    view->update();
  };
  for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
    const QString key = it.key();
    QObject::connect(it.value(), &QLineEdit::textChanged,
                     &dialog, [&changed, key, update](const QString &) {
      changed.insert(key); update();
    });
  }
  update();
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->setObjectName("seExprSpectrumButtons");
  layout->addWidget(buttons);
  bool accepted = false;
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    for (auto *field : fields) {
      if (field->hasAcceptableInput()) continue;
      field->setFocus(); return;
    }
    const auto points = spectrumPoints(read());
    if (points.isEmpty()) return;
    control["type"] = "curve"; control["presentation"] = "spectrum";
    control["spectrum"] = read(); control["points"] = points;
    accepted = true; dialog.accept();
  });
  dialog.exec();
  return accepted;
}

bool editAnimationCurve(QWidget *parent, QJsonObject &control,
                         const AnimationCurveLinkResolver &resolver) {
  QJsonObject workingBase = control;
  QDialog dialog(parent);
  dialog.setObjectName("seExprAnimationCurveDialog");
  dialog.setWindowTitle(QObject::tr("Animation Curve"));
  dialog.resize(1040, 560);
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  auto *before = new QComboBox(&dialog), *after = new QComboBox(&dialog);
  before->setObjectName("seExprAnimationPreInfinity");
  after->setObjectName("seExprAnimationPostInfinity");
  const QStringList infinityModes{"constant", "linear", "cycle", "cycleRelative", "oscillate"};
  const auto initInfinity = [&](QComboBox *combo, const QString &key) {
    combo->clear();
    for (const auto &mode : infinityModes) combo->addItem(mode, mode);
    const QString value = workingBase.value(key).toString("constant");
    int selected = combo->findData(value);
    if (selected < 0) { combo->addItem(value, value); selected = combo->count() - 1; }
    combo->setCurrentIndex(selected);
  };
  initInfinity(before, "preInfinity"); initInfinity(after, "postInfinity");
  auto *weighted = new QCheckBox(QObject::tr("Weighted Tangents"), &dialog);
  weighted->setObjectName("seExprAnimationWeighted");
  weighted->setChecked(workingBase.value("weighted").toBool());
  auto *link = new QLineEdit(workingBase.value("link").toString(), &dialog);
  link->setObjectName("seExprAnimationLink");
  link->setToolTip(QObject::tr("Native channel identifier for an explicit linked-curve refresh."));
  form->addRow(QObject::tr("Before First Key"), before);
  form->addRow(QObject::tr("After Last Key"), after);
  form->addRow(weighted);
  form->addRow(QObject::tr("Linked Curve"), link);
  auto *refresh = new QPushButton(QObject::tr("Refresh Linked Curve"), &dialog);
  refresh->setObjectName("seExprAnimationRefresh"); refresh->setEnabled(bool(resolver));
  form->addRow(refresh);
  auto *table = new QTableWidget(0, 9, &dialog);
  table->setObjectName("seExprAnimationKeys");
  table->setHorizontalHeaderLabels({QObject::tr("Time"), QObject::tr("Value"),
      QObject::tr("In Weight"), QObject::tr("Out Weight"), QObject::tr("In Angle (Degrees)"),
      QObject::tr("Out Angle (Degrees)"), QObject::tr("In Tangent"), QObject::tr("Out Tangent"),
      QObject::tr("Lock Weights")});
  table->horizontalHeader()->setStretchLastSection(true);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setItemDelegateForColumn(6, new AnimationTangentDelegate(table));
  table->setItemDelegateForColumn(7, new AnimationTangentDelegate(table));
  layout->addWidget(table);
  auto *preview = new AnimationCurveView(&dialog);
  layout->addWidget(preview);
  auto *error = new QLabel(&dialog);
  error->setObjectName("seExprAnimationError"); error->setWordWrap(true);
  layout->addWidget(error);
  const QStringList numericNames{"time", "value", "inWeight", "outWeight", "inAngle", "outAngle"};
  const QStringList tangentNames{"inTangent", "outTangent"};
  bool keysChanged = false;
  QSet<QString> changed;
  QSet<QString> keyIds;
  const auto originalKeys = workingBase.value("keys").toArray();
  for (const auto &entry : originalKeys) {
    const auto id = entry.toObject().value("id").toString();
    if (!id.isEmpty()) keyIds.insert(id);
  }
  const auto newId = [&] {
    QString id;
    do { id = "k_" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'); }
    while (keyIds.contains(id));
    keyIds.insert(id); return id;
  };
  const auto append = [&](const QJsonObject &key, const QString &id) {
    const QSignalBlocker block(table);
    const auto defaults = animationKey();
    const int row = table->rowCount();
    table->insertRow(row);
    for (int column = 0; column < numericNames.size(); ++column) {
      const auto field = numericNames[column];
      const double value = key.value(field).toDouble(defaults.value(field).toDouble());
      const QString text = table->locale().toString(value, 'g', std::numeric_limits<double>::max_digits10);
      auto *item = new QTableWidgetItem(text);
      item->setData(Qt::UserRole, text);
      table->setItem(row, column, item);
    }
    table->item(row, 0)->setData(Qt::UserRole + 1,
        QJsonDocument(key).toJson(QJsonDocument::Compact));
    table->item(row, 0)->setData(Qt::UserRole + 2, id);
    for (int i = 0; i < tangentNames.size(); ++i) {
      const QString text = key.value(tangentNames[i]).toString("auto");
      auto *item = new QTableWidgetItem(text);
      item->setData(Qt::UserRole, text); table->setItem(row, i + 6, item);
    }
    auto *locked = new QTableWidgetItem;
    locked->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    const bool value = key.value("weightsLocked").toBool();
    locked->setCheckState(value ? Qt::Checked : Qt::Unchecked);
    locked->setData(Qt::UserRole, value); table->setItem(row, 8, locked);
  };
  for (int i = 0; i < originalKeys.size(); ++i) {
    const auto key = originalKeys[i].toObject();
    QString id = key.value("id").toString();
    if (id.isEmpty()) {
      id = "key_" + QString::number(i);
      while (keyIds.contains(id)) id += '_';
      keyIds.insert(id);
    }
    append(key, id);
  }
  const auto read = [&](QJsonObject &draft, QString &message) {
    draft = workingBase;
    if (changed.contains("preInfinity")) draft["preInfinity"] = before->currentData().toString();
    if (changed.contains("postInfinity")) draft["postInfinity"] = after->currentData().toString();
    if (changed.contains("weighted")) draft["weighted"] = weighted->isChecked();
    if (changed.contains("link")) draft["link"] = link->text();
    if (keysChanged) {
      QJsonArray keys;
      for (int row = 0; row < table->rowCount(); ++row) {
        auto *time = table->item(row, 0);
        QJsonObject key = QJsonDocument::fromJson(time->data(Qt::UserRole + 1).toByteArray()).object();
        key["id"] = time->data(Qt::UserRole + 2).toString();
        for (int column = 0; column < numericNames.size(); ++column) {
          const auto *item = table->item(row, column);
          if (item->text() == item->data(Qt::UserRole).toString()) continue;
          bool valid = false;
          const double value = table->locale().toDouble(item->text(), &valid);
          if (!valid || !std::isfinite(value)) {
            message = QObject::tr("Use finite numbers for every animation key field."); return false;
          }
          key[numericNames[column]] = value;
        }
        for (int column = 0; column < tangentNames.size(); ++column) {
          const auto *item = table->item(row, column + 6);
          if (item->text() != item->data(Qt::UserRole).toString())
            key[tangentNames[column]] = item->text();
        }
        const auto *locked = table->item(row, 8);
        const bool value = locked->checkState() == Qt::Checked;
        if (value != locked->data(Qt::UserRole).toBool()) key["weightsLocked"] = value;
        keys.append(key);
      }
      draft["keys"] = keys;
    }
    return validateAnimationCurve(draft, &message);
  };
  const auto update = [&] {
    QJsonObject draft; QString message;
    TSeExprAnimCurve::Curve model;
    if (!read(draft, message) || !animationCurveModel(draft, model, message)) {
      preview->clear(message); error->setText(message);
    } else {
      error->setText(preview->setCurve(model));
    }
  };
  QObject::connect(table, &QTableWidget::itemChanged, &dialog, [&](QTableWidgetItem *item) {
    if ((item->column() == 2 || item->column() == 3) &&
        table->item(item->row(), 8)->checkState() == Qt::Checked) {
      const QSignalBlocker block(table);
      table->item(item->row(), item->column() == 2 ? 3 : 2)->setText(item->text());
    }
    keysChanged = true; update();
  });
  QObject::connect(before, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
      [&](int) { changed.insert("preInfinity"); update(); });
  QObject::connect(after, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
      [&](int) { changed.insert("postInfinity"); update(); });
  QObject::connect(weighted, &QCheckBox::toggled, &dialog,
      [&](bool) { changed.insert("weighted"); update(); });
  QObject::connect(link, &QLineEdit::textChanged, &dialog,
      [&](const QString &) { changed.insert("link"); update(); });
  QObject::connect(refresh, &QPushButton::clicked, &dialog, [&] {
    if (!resolver) return;
    QJsonObject draft; QString message;
    try {
      if (!read(draft, message) || !resolver(link->text(), draft, message) ||
          !validateAnimationCurve(draft, &message)) {
        error->setText(message); return;
      }
    } catch (...) {
      error->setText(QObject::tr("The native linked curve could not be refreshed.")); return;
    }
    workingBase = draft;
    const QSignalBlocker blockTable(table), blockBefore(before), blockAfter(after),
        blockWeighted(weighted), blockLink(link);
    keyIds.clear();
    for (const auto &entry : draft.value("keys").toArray()) {
      const auto id = entry.toObject().value("id").toString();
      if (!id.isEmpty()) keyIds.insert(id);
    }
    for (const auto &entry : draft.value("nativeKeys").toArray()) {
      const auto id = entry.toObject().value("id").toString();
      if (!id.isEmpty()) keyIds.insert(id);
    }
    table->setRowCount(0);
    const auto keys = draft.value("keys").toArray();
    for (int i = 0; i < keys.size(); ++i) {
      const auto key = keys[i].toObject();
      QString id = key.value("id").toString();
      if (id.isEmpty()) {
        id = "key_" + QString::number(i);
        while (keyIds.contains(id)) id += '_';
        keyIds.insert(id);
      }
      append(key, id);
    }
    initInfinity(before, "preInfinity"); initInfinity(after, "postInfinity");
    weighted->setChecked(draft.value("weighted").toBool()); link->setText(draft.value("link").toString());
    changed.clear(); keysChanged = false; update();
  });
  auto *operations = new QHBoxLayout;
  layout->addLayout(operations);
  const auto button = [&](const QString &text, const QString &name) {
    auto *result = new QPushButton(text, &dialog); result->setObjectName(name);
    operations->addWidget(result); return result;
  };
  auto *add = button(QObject::tr("Add Key"), "seExprAnimationAdd");
  auto *duplicate = button(QObject::tr("Duplicate Key"), "seExprAnimationDuplicate");
  auto *remove = button(QObject::tr("Remove Key"), "seExprAnimationRemove");
  auto *up = button(QObject::tr("Move Up"), "seExprAnimationUp");
  auto *down = button(QObject::tr("Move Down"), "seExprAnimationDown");
  auto *sort = button(QObject::tr("Sort by Time"), "seExprAnimationSort");
  QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
    if (table->rowCount() >= 64) {
      error->setText(QObject::tr("A native animation curve supports at most 64 keys.")); return;
    }
    double time = 0;
    if (table->rowCount()) {
      bool valid = false;
      time = table->locale().toDouble(table->item(table->rowCount() - 1, 0)->text(), &valid);
      if (!valid || !std::isfinite(time + 1) || !(time + 1 > time)) {
        error->setText(QObject::tr("Choose a finite, distinct time for the last key before adding another.")); return;
      }
      time += 1;
    }
    auto key = animationKey(time); const auto id = newId(); key["id"] = id;
    append(key, id); keysChanged = true;
    table->setCurrentCell(table->rowCount() - 1, 0); update();
  });
  QObject::connect(duplicate, &QPushButton::clicked, &dialog, [&] {
    const int row = table->currentRow();
    if (row < 0) return;
    if (table->rowCount() >= 64) {
      error->setText(QObject::tr("A native animation curve supports at most 64 keys.")); return;
    }
    QJsonObject draft; QString message;
    if (!read(draft, message)) { error->setText(message); return; }
    auto key = draft.value("keys").toArray()[row].toObject();
    // Preserve all authored fields, but give the duplicate a distinct owner.
    const auto id = newId(); key["id"] = id;
    append(key, id); keysChanged = true;
    table->setCurrentCell(table->rowCount() - 1, 0); update();
  });
  QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
    const int row = table->currentRow(); if (row < 0) return;
    table->removeRow(row); keysChanged = true; update();
  });
  const auto move = [&](int direction) {
    const int row = table->currentRow(), target = row + direction;
    if (row < 0 || target < 0 || target >= table->rowCount()) return;
    const QSignalBlocker block(table);
    for (int column = 0; column < table->columnCount(); ++column) {
      auto *first = table->takeItem(row, column), *second = table->takeItem(target, column);
      table->setItem(row, column, second); table->setItem(target, column, first);
    }
    table->setCurrentCell(target, 0); keysChanged = true; update();
  };
  QObject::connect(up, &QPushButton::clicked, &dialog, [move] { move(-1); });
  QObject::connect(down, &QPushButton::clicked, &dialog, [move] { move(1); });
  QObject::connect(sort, &QPushButton::clicked, &dialog, [&] {
    QVector<int> order;
    QVector<double> times;
    for (int row = 0; row < table->rowCount(); ++row) {
      bool valid = false;
      const double time = table->locale().toDouble(table->item(row, 0)->text(), &valid);
      if (!valid || !std::isfinite(time)) {
        error->setText(QObject::tr("Use finite key times before sorting.")); return;
      }
      order.append(row); times.append(time);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return times[a] < times[b]; });
    const QSignalBlocker block(table);
    QVector<QVector<QTableWidgetItem *>> rows;
    for (int row = 0; row < table->rowCount(); ++row) {
      QVector<QTableWidgetItem *> cells;
      for (int column = 0; column < table->columnCount(); ++column)
        cells.append(table->takeItem(row, column));
      rows.append(cells);
    }
    for (int row = 0; row < order.size(); ++row)
      for (int column = 0; column < table->columnCount(); ++column)
        table->setItem(row, column, rows[order[row]][column]);
    keysChanged = true; update();
  });
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->setObjectName("seExprAnimationButtons"); layout->addWidget(buttons);
  bool accepted = false;
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    QJsonObject draft; QString message;
    if (!read(draft, message)) { error->setText(message); return; }
    draft["type"] = "animcurve"; control = draft;
    accepted = true; dialog.accept();
  });
  update(); dialog.exec();
  return accepted;
}
}  // namespace SeExprControls
