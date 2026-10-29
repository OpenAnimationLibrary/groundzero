#include "toonzqt/seexprcontrols.h"
#include "tseexprspectrum.h"

#include <QColorDialog>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace SeExprControls {
namespace {
QStringList arguments(const QString &source) {
  QStringList values;
  int start = 0, depth = 0;
  QChar quote;
  bool escaped = false;
  for (int i = 0; i < source.size(); ++i) {
    const QChar c = source[i];
    if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
    } else if (c == '"' || c == '\'') quote = c;
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

QJsonObject controlFor(const QString &expression, const QString &comment,
                       QString &input) {
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
}  // namespace

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
    bool innerEscaped = false;
    for (; end < source.size(); ++end) {
      const QChar q = source[end];
      if (!innerQuote.isNull()) {
        if (innerEscaped) innerEscaped = false;
        else if (q == '\\') innerEscaped = true;
        else if (q == innerQuote) innerQuote = QChar();
      } else if (q == '"' || q == '\'') innerQuote = q;
      else if (q == '(' || q == '[' || q == '{') ++depth;
      else if (q == ')' || q == ']' || q == '}') --depth;
      else if (depth == 0 && (q == ';' || q == '#' || q == '\n')) break;
    }
    QString input;
    QJsonObject control = controlFor(source.mid(begin, end - begin).trimmed(),
                                    annotation(source, end), input);
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
  QMap<QString, QDoubleSpinBox *> fields;
  const QStringList keys{"resolution", "tileSize", "lengthCutoff", "amplitude",
      "windAngle", "windSpeed", "directionalFactorExponent", "directionalReflectionDamping",
      "sharpen", "time", "filterWidth"};
  const QStringList labels{QObject::tr("Resolution Exponent"), QObject::tr("Tile Size"),
      QObject::tr("Length Cutoff"), QObject::tr("Amplitude"), QObject::tr("Wind Angle"),
      QObject::tr("Wind Speed"), QObject::tr("Directional Exponent"),
      QObject::tr("Reflection Damping"), QObject::tr("Sharpen"), QObject::tr("Time"),
      QObject::tr("Filter Width")};
  for (int i = 0; i < keys.size(); ++i) {
    auto *field = new QDoubleSpinBox(&dialog);
    field->setObjectName("seExprSpectrum_" + keys[i]);
    field->setDecimals(keys[i] == "resolution" ? 0 : 6);
    field->setRange(0, 1000000);
    if (keys[i] == "resolution") field->setRange(1, 20);
    if (keys[i] == "windAngle" || keys[i] == "time") field->setMinimum(-1000000);
    if (keys[i] == "directionalFactorExponent") field->setRange(0, 32);
    if (keys[i] == "windSpeed" || keys[i] == "tileSize") field->setMinimum(0.000001);
    field->setValue(params.value(keys[i]).toDouble(defaultSpectrumParameters().value(keys[i]).toDouble()));
    fields.insert(keys[i], field);
    form->addRow(labels[i], field);
  }
  const auto flow = params.value("flowDirection").toArray();
  for (int i = 0; i < 3; ++i) {
    auto *field = new QDoubleSpinBox(&dialog);
    field->setObjectName("seExprSpectrum_flow" + QString::number(i));
    field->setDecimals(6); field->setRange(-1000000, 1000000);
    field->setValue(flow.size() > i ? flow[i].toDouble() : 0);
    fields.insert("flow" + QString::number(i), field);
    form->addRow(QObject::tr("Flow Component %1").arg(i + 1), field);
  }
  auto *view = new SpectrumView(&dialog);
  layout->addWidget(view);
  const auto read = [&] {
    QJsonObject current = params;
    for (const auto &key : keys) current[key] = fields[key]->value();
    current["flowDirection"] = QJsonArray{fields["flow0"]->value(), fields["flow1"]->value(), fields["flow2"]->value()};
    return current;
  };
  const auto update = [&] {
    const auto current = read();
    view->points = spectrumPoints(current);
    view->grid = TSeExprSpectrum::gridRange(spectrumParameters(current));
    view->update();
  };
  for (auto *field : fields)
    QObject::connect(field, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                     &dialog, [update](double) { update(); });
  update();
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->setObjectName("seExprSpectrumButtons");
  layout->addWidget(buttons);
  bool accepted = false;
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    const auto points = spectrumPoints(read());
    if (points.isEmpty()) return;
    control["type"] = "curve"; control["presentation"] = "spectrum";
    control["spectrum"] = read(); control["points"] = points;
    accepted = true; dialog.accept();
  });
  dialog.exec();
  return accepted;
}
}  // namespace SeExprControls
