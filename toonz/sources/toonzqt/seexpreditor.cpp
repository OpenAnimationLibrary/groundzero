#include "toonzqt/seexpreditor.h"
#include "toonzqt/seexprcontrols.h"
#include "toonzqt/seexprrangeviewer.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace {
constexpr int MaxPresetBytes = 262144;
QString text(const std::string &value) { return QString::fromUtf8(value.c_str()); }
std::string bytes(const QString &value) { return value.toUtf8().toStdString(); }
QString compact(const QJsonObject &value) {
  return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}
QJsonObject defaultDocument() {
  return {{"version", 1}, {"backend", "interpreter"}, {"rgb", "[$u,$v,0.5]"}, {"alpha", "1"},
          {"controls", QJsonArray()}};
}
bool identifier(const QString &value) {
  static const QRegularExpression pattern("^[A-Za-z_][A-Za-z0-9_]*$");
  return pattern.match(value).hasMatch();
}
bool validControlShape(const QJsonObject &control) {
  const QString type = control.value("type").toString();
  if (!identifier(control.value("id").toString()) ||
      !identifier(control.value("name").toString())) return false;
  const auto finiteNumber = [](const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble());
  };
  const auto vector = [&finiteNumber](const QJsonValue &value) {
    if (!value.isArray() || value.toArray().size() != 3) return false;
    for (const auto &entry : value.toArray()) if (!finiteNumber(entry)) return false;
    return true;
  };
  const auto value = control.value("value");
  if (type == "scalar") return finiteNumber(value);
  if (type == "vector" || type == "color") return vector(value);
  if (type == "string") return value.isString();
  if (type == "animcurve") return SeExprControls::validateAnimationCurve(control);
  if (type != "curve" && type != "colorcurve") return false;
  const auto points = control.value("points").toArray();
  if (points.size() < 2 || points.size() > 64) return false;
  double previous = -1.;
  for (const auto &entry : points) {
    if (!entry.isArray() || entry.toArray().size() < 2 || entry.toArray().size() > 3) return false;
    const auto point = entry.toArray();
    const double position = point[0].toDouble(-1.);
    if (!finiteNumber(point[0]) || position < 0. || position > 1. || position <= previous ||
        !(type == "colorcurve" ? vector(point[1]) : finiteNumber(point[1]))) return false;
    if (point.size() == 3 && (!finiteNumber(point[2]) || point[2].toDouble() != point[2].toInt() ||
                             point[2].toInt() < 0 || point[2].toInt() > 4)) return false;
    previous = position;
  }
  return true;
}
QString valueText(const QJsonValue &value) {
  if (value.isString()) return value.toString();
  if (value.isDouble()) return QString::number(value.toDouble(), 'g', 17);
  return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
}

class CodeEdit final : public QPlainTextEdit {
  QCompleter *m_completer;
  QStringListModel *m_model;
public:
  explicit CodeEdit(QWidget *parent) : QPlainTextEdit(parent) {
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    m_model = new QStringListModel(this);
    m_completer = new QCompleter(m_model, this);
    m_completer->setWidget(this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    QObject::connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, [this](const QString &completion) {
      QTextCursor cursor = textCursor();
      cursor.select(QTextCursor::WordUnderCursor);
      cursor.insertText(completion);
      setTextCursor(cursor);
    });
  }
  void setWords(const QStringList &words) {
    QStringList unique = words;
    unique.removeDuplicates();
    unique.sort(Qt::CaseInsensitive);
    m_model->setStringList(unique);
  }
protected:
  void keyPressEvent(QKeyEvent *event) override {
    if (m_completer->popup()->isVisible() &&
        (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return ||
         event->key() == Qt::Key_Escape || event->key() == Qt::Key_Tab)) {
      event->ignore();
      return;
    }
    const bool explicitCompletion = event->key() == Qt::Key_Space &&
                                    (event->modifiers() & Qt::ControlModifier);
    if (!explicitCompletion) QPlainTextEdit::keyPressEvent(event);
    QTextCursor cursor = textCursor();
    cursor.select(QTextCursor::WordUnderCursor);
    const QString prefix = cursor.selectedText();
    if ((!explicitCompletion && (event->text().isEmpty() || prefix.size() < 2)) ||
        (event->modifiers() & Qt::AltModifier)) {
      m_completer->popup()->hide();
      return;
    }
    m_completer->setCompletionPrefix(prefix);
    QRect rect = cursorRect();
    rect.setWidth(std::max(180, m_completer->popup()->sizeHintForColumn(0) + 24));
    m_completer->complete(rect);
  }
};

class Highlighter final : public QSyntaxHighlighter {
public:
  explicit Highlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}
protected:
  void highlightBlock(const QString &line) override {
    const bool dark = QApplication::palette().color(QPalette::Base).lightness() < 128;
    const auto mark = [this, &line](const QString &pattern, const QColor &color,
                                    bool bold = false) {
      QTextCharFormat format;
      format.setForeground(color);
      if (bold) format.setFontWeight(QFont::Bold);
      auto matches = QRegularExpression(pattern).globalMatch(line);
      while (matches.hasNext()) {
        const auto match = matches.next();
        setFormat(match.capturedStart(), match.capturedLength(), format);
      }
    };
    mark("\\b(if|else|extern|def|float|string|constant|uniform|varying)\\b",
         dark ? QColor(190, 160, 250) : QColor(90, 40, 145), true);
    mark("\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()",
         dark ? QColor(100, 200, 240) : QColor(0, 95, 135));
    mark("\\$[A-Za-z_][A-Za-z0-9_]*", dark ? QColor(240, 200, 130) : QColor(130, 75, 0));
    mark("\\b[0-9]+(?:\\.[0-9]*)?(?:[eE][+-]?[0-9]+)?\\b",
         dark ? QColor(150, 220, 170) : QColor(25, 110, 55));
    // Strings and comments override token coloring. A # in a string is literal.
    bool quoted = false, escaped = false;
    QChar quote;
    int start = -1;
    for (int i = 0; i < line.size(); ++i) {
      const QChar c = line[i];
      if (quoted) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == quote) {
          setFormat(start, i - start + 1, dark ? QColor(175, 220, 170) : QColor(35, 115, 40));
          quoted = false;
        }
      } else if (c == '"' || c == '\'') { quoted = true; quote = c; start = i; }
      else if (c == '#') {
        setFormat(i, line.size() - i, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        break;
      }
    }
  }
};

class CurvePreview final : public QWidget {
  QJsonArray m_points;
  bool m_color = false;
  std::vector<TSeExpr::Value> m_samples;
  QString m_error;
public:
  explicit CurvePreview(QWidget *parent) : QWidget(parent) { setMinimumHeight(90); }
  void setPoints(const QJsonArray &points, bool color) {
    m_points = points; m_color = color; m_samples.clear(); m_error.clear();
    QString expression = color ? "ccurve($lookup" : "curve($lookup";
    for (const auto &entry : points) {
      const auto point = entry.toArray();
      if (point.size() < 2) continue;
      expression += "," + valueText(point[0]) + "," + valueText(point[1]) + "," +
                    QString::number(point.size() >= 3 ? point[2].toInt(1) : 1);
    }
    expression += ")";
    TSeExpr::Options options; options.resultType = color ? TSeExpr::Type::Vector : TSeExpr::Type::Scalar;
    TSeExpr::Program program;
    if (!program.compile(bytes(expression), {{"lookup", TSeExpr::Value::scalar(0.)}}, {}, options))
      m_error = text(program.errorMessage());
    else {
      for (int sample = 0; sample <= 128; ++sample) {
        program.setScalar("lookup", sample / 128.);
        TSeExpr::Value result;
        if (!program.evaluate(result)) { m_error = text(program.errorMessage()); m_samples.clear(); break; }
        m_samples.push_back(std::move(result));
      }
    }
    update();
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    const QRect area = rect().adjusted(8, 8, -8, -8);
    painter.fillRect(area, palette().brush(QPalette::Base));
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(area);
    if (m_points.isEmpty()) return;
    if (!m_error.isEmpty()) { painter.drawText(area.adjusted(4, 4, -4, -4), Qt::TextWordWrap, m_error); return; }
    for (int component = 0; component < (m_color ? 3 : 1); ++component) {
      painter.setPen(m_color ? QColor(component == 0 ? 230 : 80, component == 1 ? 210 : 80,
                                     component == 2 ? 230 : 80) : palette().color(QPalette::Highlight));
      QPointF previous;
      bool first = true;
      for (std::size_t sample = 0; sample < m_samples.size(); ++sample) {
        const double value = m_samples[sample].number[component];
        const QPointF next(area.left() + double(sample) / 128. * area.width(),
                           area.bottom() - std::clamp(value, 0., 1.) * area.height());
        if (!first) painter.drawLine(previous, next);
        previous = next; first = false;
      }
    }
  }
};

QJsonArray editPoints(QWidget *parent, const QJsonArray &initial, bool color,
                      bool &accepted) {
  QDialog dialog(parent);
  dialog.setWindowTitle(SeExprEditor::tr("Edit Curve Points"));
  auto layout = new QVBoxLayout(&dialog);
  auto preview = new CurvePreview(&dialog);
  layout->addWidget(preview);
  auto table = new QTableWidget(initial.size(), color ? 5 : 3, &dialog);
  table->setHorizontalHeaderLabels(color ? QStringList{SeExprEditor::tr("Position"), "R", "G", "B", SeExprEditor::tr("Interpolation")}
                                         : QStringList{SeExprEditor::tr("Position"), SeExprEditor::tr("Value"), SeExprEditor::tr("Interpolation")});
  table->setToolTip(SeExprEditor::tr("Interpolation: 0 hold, 1 linear, 2 smooth, 3 spline, 4 monotone."));
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  for (int row = 0; row < initial.size(); ++row) {
    const auto point = initial[row].toArray();
    for (int column = 0; column < table->columnCount(); ++column) {
      const double value = column == table->columnCount() - 1 ? (point.size() >= 3 ? point[2].toInt(1) : 1.) : column == 0 ? point[0].toDouble()
          : color ? point[1].toArray().at(column - 1).toDouble() : point[1].toDouble();
      table->setItem(row, column, new QTableWidgetItem(QString::number(value, 'g', 17)));
    }
  }
  const auto points = [table, color]() {
    QJsonArray result;
    for (int row = 0; row < table->rowCount(); ++row) {
      QJsonArray point, rgb;
      int interpolation = 1;
      for (int column = 0; column < table->columnCount(); ++column) {
        bool ok = false;
        const double value = table->item(row, column)
            ? table->item(row, column)->text().toDouble(&ok) : 0.;
        if (!ok || !std::isfinite(value)) return QJsonArray();
        if (column == table->columnCount() - 1) {
          if (value != int(value) || value < 0 || value > 4) return QJsonArray();
          interpolation = int(value);
        } else if (column == 0) point.append(value);
        else if (color) rgb.append(value);
        else point.append(value);
      }
      if (color) point.append(rgb);
      point.append(interpolation);
      result.append(point);
    }
    return result;
  };
  QObject::connect(table, &QTableWidget::itemChanged, &dialog,
          [preview, points, color]() { preview->setPoints(points(), color); });
  preview->setPoints(initial, color);
  layout->addWidget(table);
  auto rowButtons = new QHBoxLayout();
  auto add = new QPushButton(SeExprEditor::tr("Add Point"), &dialog);
  auto remove = new QPushButton(SeExprEditor::tr("Remove Point"), &dialog);
  rowButtons->addWidget(add); rowButtons->addWidget(remove); rowButtons->addStretch();
  layout->addLayout(rowButtons);
  QObject::connect(add, &QPushButton::clicked, &dialog, [table, color]() {
    if (table->rowCount() >= 64) return;
    const int row = table->rowCount(); table->insertRow(row);
    for (int column = 0; column < table->columnCount(); ++column)
      table->setItem(row, column, new QTableWidgetItem(column == table->columnCount() - 1 ? "1" : column == 0 ? "0.5" : color ? "1" : "0.5"));
  });
  QObject::connect(remove, &QPushButton::clicked, &dialog, [table]() {
    if (table->currentRow() >= 0) table->removeRow(table->currentRow());
  });
  auto warning = new QLabel(&dialog); warning->setWordWrap(true); layout->addWidget(warning);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, warning, points]() {
    const auto result = points();
    double previous = -1.;
    bool valid = result.size() >= 2;
    for (const auto &entry : result) {
      const auto point = entry.toArray(); const double position = point[0].toDouble();
      valid = valid && position >= 0. && position <= 1. && position > previous;
      previous = position;
    }
    if (!valid) { warning->setText(SeExprEditor::tr("Use at least two finite points in increasing positions from 0 to 1.")); return; }
    dialog.accept();
  });
  accepted = dialog.exec() == QDialog::Accepted;
  return accepted ? points() : initial;
}
}  // namespace

class SeExprEditor::Imp {
public:
  SeExprEditor *owner;
  QTabWidget *tabs;
  CodeEdit *rgb, *alpha, *displacement, *opacity;
  QTableWidget *controls;
  QListWidget *presets, *functions;
  QTextBrowser *help;
  QLabel *status, *preview;
  QLineEdit *presetFilter, *functionFilter;
  QPushButton *apply;
  QTimer *debounce;
  QWidget *backendRow;
  QComboBox *backend;
  QJsonObject object = defaultDocument();
  QString committed;
  QString presetDirectory;
  QString builtinPresetDirectory;
  bool documentMode = true, loading = false;
  bool surfaceSupported = true;
  TSeExpr::Type type = TSeExpr::Type::Vector;
  double frame = 0.;
  std::vector<TSeExpr::Variable> variables;
  std::vector<TSeExpr::Function> customFunctions;
  std::function<QString(const QString &)> validator, evaluator;
  std::function<bool(const QString &, QString &, QString &)> checkedEvaluator;
  std::function<SeExprEditor::RangeEvaluator(const QString &)> rangeFactory;
  SeExprEditor::AnimationCurveLinkResolver curveLinkResolver;
  std::vector<TSeExpr::FunctionInfo> catalog;

  explicit Imp(SeExprEditor *widget) : owner(widget) {}

  void changed() {
    if (loading) return;
    apply->setEnabled(owner->isModified());
    status->setText(SeExprEditor::tr("Draft changed. Validate or Apply to evaluate it."));
    emit owner->modifiedChanged(owner->isModified());
    debounce->start();
  }
  QJsonObject currentObject() const {
    QJsonObject result = object;
    result["rgb"] = rgb->toPlainText();
    result["alpha"] = alpha->toPlainText();
    if (object.contains("displacement"))
      result["displacement"] = displacement->toPlainText();
    if (object.contains("opacity")) result["opacity"] = opacity->toPlainText();
    result["backend"] = backend->currentData().toString();
    return result;
  }
  void loadObject(const QJsonObject &value) {
    loading = true;
    object = value;
    if (object.contains("displacement") && !object.contains("opacity"))
      object["opacity"] = "[1,1,1]";
    const QString selectedBackend = value.value("backend").toString("interpreter");
    int backendIndex = backend->findData(selectedBackend);
    if (backendIndex < 0) {
      backend->addItem(SeExprEditor::tr("Unsupported backend (%1)").arg(selectedBackend), selectedBackend);
      backendIndex = backend->count() - 1;
    }
    backend->setCurrentIndex(backendIndex);
    rgb->setPlainText(value.value("rgb").toString());
    alpha->setPlainText(value.value("alpha").toString("1"));
    displacement->setPlainText(value.value("displacement").toString("0"));
    opacity->setPlainText(object.value("opacity").toString("[1,1,1]"));
    tabs->setTabVisible(tabs->indexOf(displacement),
                        surfaceSupported && documentMode && object.contains("displacement"));
    tabs->setTabVisible(tabs->indexOf(opacity),
                        surfaceSupported && documentMode && object.contains("opacity"));
    refreshControls();
    loading = false;
    apply->setEnabled(owner->isModified());
    emit owner->modifiedChanged(owner->isModified());
  }
  void refreshControls() {
    const auto values = object.value("controls").toArray();
    controls->setRowCount(values.size());
    for (int row = 0; row < values.size(); ++row) {
      const auto control = values[row].toObject();
      const QStringList columns = {control.value("label").toString(control.value("name").toString()),
        control.value("name").toString(), control.value("type").toString(),
        valueText(control.value("value"))};
      for (int column = 0; column < columns.size(); ++column) {
        auto item = new QTableWidgetItem(columns[column]);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        controls->setItem(row, column, item);
      }
    }
    refreshCompletion();
  }
  void refreshCompletion() {
    QStringList names;
    for (const auto &entry : catalog) names << text(entry.name);
    for (const auto &entry : customFunctions) names << text(entry.name);
    for (const auto &entry : variables) names << text(entry.name);
    for (const auto &entry : object.value("controls").toArray())
      names << entry.toObject().value("name").toString();
    names << "if" << "else" << "def" << "extern" << "float" << "string";
    rgb->setWords(names); alpha->setWords(names); displacement->setWords(names);
    opacity->setWords(names);
  }
  void refreshFunctions() {
    functions->clear();
    const QString filter = functionFilter->text();
    const auto append = [this, &filter](const QString &name, const QString &doc) {
      if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive) &&
          !doc.contains(filter, Qt::CaseInsensitive)) return;
      auto item = new QListWidgetItem(name, functions);
      item->setData(Qt::UserRole, doc);
    };
    for (const auto &entry : catalog) append(text(entry.name), text(entry.documentation));
    for (const auto &entry : customFunctions) append(text(entry.name), text(entry.documentation));
    refreshCompletion();
  }
  void refreshPresets() {
    presets->clear();
    const std::pair<const char *, const char *> builtins[] = {
      {"UV Gradient", "[$u,$v,0.5]"}, {"Checker", "fmod(floor($u*8)+floor($v*8),2)"},
      {"Stripes", "0.5+0.5*sin($u*40)"}, {"Clouds", "fbm([$u*6,$v*6,$time])"},
      {"Cell Pattern", "cellnoise([$u*16,$v*16,$seed])"},
      {"Animated Matte", "smoothstep(noise([$u*4,$v*4,$time]),0.3,0.7)"},
      {"Source Color", "$src"}, {"Channel Swap", "[$src[2],$src[1],$src[0]]"},
      {"Vignette", "$src*(1-clamp(length([$u-0.5,$v-0.5,0])*1.4,0,1))"}};
    for (const auto &entry : builtins) {
      if (!documentMode) break;
      QJsonObject preset = defaultDocument(); preset["rgb"] = entry.second;
      const QString name = SeExprEditor::tr(entry.first);
      if (!name.contains(presetFilter->text(), Qt::CaseInsensitive)) continue;
      auto item = new QListWidgetItem(name, presets);
      item->setData(Qt::UserRole, compact(preset));
    }
    if (!documentMode) {
      const std::pair<const char *, const char *> animation[] = {
        {"Constant", "1"}, {"Sine Wave", "sin($frame*0.1)"},
        {"Noise Motion", "noise([$frame*0.05,0,0])"},
        {"Smooth Step", "smoothstep($frame,1,24)"}};
      for (const auto &entry : animation) {
        const QString name = SeExprEditor::tr(entry.first);
        if (!name.contains(presetFilter->text(), Qt::CaseInsensitive)) continue;
        auto preset = defaultDocument(); preset["rgb"] = type == TSeExpr::Type::String ? "\"text\"" : entry.second;
        auto item = new QListWidgetItem(name, presets); item->setData(Qt::UserRole, compact(preset));
      }
    }
    for (const QString &location : {builtinPresetDirectory, presetDirectory}) {
      if (location.isEmpty()) continue;
      QDir directory(location);
      for (const auto &name : directory.entryList({"*.json"}, QDir::Files, QDir::Name)) {
      QFile file(directory.filePath(name));
      if (!file.open(QIODevice::ReadOnly) || file.size() > MaxPresetBytes) continue;
      const auto doc = QJsonDocument::fromJson(file.readAll());
      if (!doc.isObject()) continue;
      const auto root = doc.object();
      const QString title = root.value("title").toString(QFileInfo(name).completeBaseName());
      if (!title.contains(presetFilter->text(), Qt::CaseInsensitive)) continue;
      auto item = new QListWidgetItem(title, presets);
      item->setData(Qt::UserRole, QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
      }
    }
  }
  void editControl(int row) {
    auto entries = object.value("controls").toArray();
    const bool adding = row < 0;
    QJsonObject control = adding ? QJsonObject{{"id", "control_" + QUuid::createUuid().toString(QUuid::Id128)},
      {"name", QString("control%1").arg(entries.size() + 1)}, {"label", ""},
      {"type", "scalar"}, {"value", 0.}, {"min", 0.}, {"max", 1.}} : entries[row].toObject();
    QDialog dialog(owner); dialog.setWindowTitle(SeExprEditor::tr("Expression Control"));
    dialog.setObjectName("seExprControlDialog");
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout(); layout->addLayout(form);
    auto label = new QLineEdit(control.value("label").toString(), &dialog);
    label->setObjectName("seExprControlLabel");
    auto name = new QLineEdit(control.value("name").toString(), &dialog);
    auto kind = new QComboBox(&dialog);
    kind->addItems({"scalar", "vector", "color", "string", "curve", "colorcurve", "animcurve"});
    kind->setCurrentText(control.value("type").toString());
    kind->setEnabled(adding);
    auto value = new QLineEdit(valueText(control.value("value")), &dialog);
    value->setObjectName("seExprControlValue");
    auto stringKind = new QComboBox(&dialog);
    stringKind->setObjectName("seExprControlStringKind");
    stringKind->addItem(SeExprEditor::tr("Text"), "string");
    stringKind->addItem(SeExprEditor::tr("File"), "file");
    stringKind->addItem(SeExprEditor::tr("Directory"), "directory");
    const int stringIndex = stringKind->findData(control.value("annotationType").toString());
    if (stringIndex >= 0) stringKind->setCurrentIndex(stringIndex);
    auto low = new QDoubleSpinBox(&dialog), high = new QDoubleSpinBox(&dialog);
    low->setObjectName("seExprControlMinimum");
    high->setObjectName("seExprControlMaximum");
    for (auto spin : {low, high}) { spin->setDecimals(6); spin->setRange(-1e9, 1e9); }
    low->setValue(control.value("min").toDouble()); high->setValue(control.value("max").toDouble(1));
    const double displayedMinimum = low->value();
    const double displayedMaximum = high->value();
    form->addRow(SeExprEditor::tr("Display Name"), label);
    form->addRow(SeExprEditor::tr("Variable"), name);
    form->addRow(SeExprEditor::tr("Type"), kind);
    form->addRow(SeExprEditor::tr("Default Value"), value);
    form->addRow(SeExprEditor::tr("String Input"), stringKind);
    form->addRow(SeExprEditor::tr("Minimum"), low); form->addRow(SeExprEditor::tr("Maximum"), high);
    auto color = new QPushButton(SeExprEditor::tr("Choose Color"), &dialog);
    color->setObjectName("seExprControlColor");
    auto curve = new QPushButton(SeExprEditor::tr("Edit Points"), &dialog);
    auto browse = new QPushButton(SeExprEditor::tr("Browse..."), &dialog);
    browse->setObjectName("seExprControlBrowse");
    form->addRow(color); form->addRow(curve); form->addRow(browse);
    QJsonArray points = control.value("points").toArray();
    const auto updateKind = [form, kind, value, color, curve, stringKind, browse, low, high, &points]() {
      const QString type = kind->currentText();
      color->setVisible(type == "color");
      curve->setVisible(type == "curve" || type == "colorcurve" || type == "animcurve");
      curve->setText(type == "animcurve" ? SeExprEditor::tr("Edit Animation Curve")
                                          : SeExprEditor::tr("Edit Points"));
      stringKind->setVisible(type == "string");
      form->labelForField(stringKind)->setVisible(type == "string");
      browse->setVisible(type == "string" && stringKind->currentData().toString() != "string");
      value->setEnabled(type != "curve" && type != "colorcurve" && type != "animcurve");
      for (auto field : {low, high}) {
        field->setVisible(type != "animcurve");
        form->labelForField(field)->setVisible(type != "animcurve");
      }
      if ((type == "vector" || type == "color") && !value->text().startsWith('[')) value->setText("[0,0,0]");
      if (points.isEmpty() && type == "curve") points = QJsonArray{QJsonArray{0., 0.}, QJsonArray{1., 1.}};
      if (points.isEmpty() && type == "colorcurve") points = QJsonArray{QJsonArray{0., QJsonArray{0., 0., 0.}}, QJsonArray{1., QJsonArray{1., 1., 1.}}};
    };
    QObject::connect(kind, &QComboBox::currentTextChanged, &dialog, [updateKind]() { updateKind(); });
    QObject::connect(stringKind, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     &dialog, [updateKind]() { updateKind(); });
    updateKind();
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, value, stringKind]() {
      const bool directory = stringKind->currentData().toString() == "directory";
      QFileDialog picker(&dialog, directory ? SeExprEditor::tr("Choose Directory")
                                            : SeExprEditor::tr("Choose File"));
      picker.setObjectName("seExprControlPathDialog");
      picker.setAcceptMode(QFileDialog::AcceptOpen);
      picker.setFileMode(directory ? QFileDialog::Directory : QFileDialog::ExistingFile);
      if (directory) picker.setOption(QFileDialog::ShowDirsOnly);
      if (!value->text().isEmpty()) picker.selectFile(value->text());
      if (picker.exec() == QDialog::Accepted && !picker.selectedFiles().isEmpty())
        value->setText(picker.selectedFiles().front());
    });
    QObject::connect(color, &QPushButton::clicked, &dialog, [&dialog, value]() {
      const auto array = QJsonDocument::fromJson(value->text().toUtf8()).array();
      const QColor initial = array.size() == 3 ? QColor::fromRgbF(std::clamp(array[0].toDouble(), 0., 1.),
          std::clamp(array[1].toDouble(), 0., 1.), std::clamp(array[2].toDouble(), 0., 1.)) : QColor(Qt::white);
      QColorDialog picker(initial, &dialog);
      // QColorDialog can quantize its input when it initializes its display
      // controls. Compare edits with the displayed baseline, not the original
      // higher-precision color supplied to the dialog.
      const QColor displayed = picker.currentColor();
      if (picker.exec() != QDialog::Accepted) return;
      const QColor selected = picker.selectedColor();
      if (selected.isValid() && selected != displayed) {
        // The picker presents a bounded display color. Retain the original
        // precision and HDR values for every component the artist leaves alone.
        const auto component = [&](int index, double chosen, double displayed) {
          return array.size() == 3 && chosen == displayed ? array[index].toDouble() : chosen;
        };
        value->setText(QString("[%1,%2,%3]")
            .arg(component(0, selected.redF(), displayed.redF()), 0, 'g', 17)
            .arg(component(1, selected.greenF(), displayed.greenF()), 0, 'g', 17)
            .arg(component(2, selected.blueF(), displayed.blueF()), 0, 'g', 17));
      }
    });
    QObject::connect(curve, &QPushButton::clicked, &dialog,
        [&dialog, &points, &control, kind, resolver = curveLinkResolver]() {
      if (kind->currentText() == "animcurve") {
        if (!control.contains("keys")) {
          const auto defaults = SeExprControls::defaultAnimationCurve();
          for (auto it = defaults.begin(); it != defaults.end(); ++it)
            if (!control.contains(it.key())) control[it.key()] = it.value();
        }
        control["type"] = "animcurve";
        SeExprControls::editAnimationCurve(&dialog, control, resolver);
        return;
      }
      bool accepted; points = editPoints(&dialog, points, kind->currentText() == "colorcurve", accepted);
    });
    auto error = new QLabel(&dialog); error->setWordWrap(true); layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    buttons->setObjectName("seExprControlButtons");
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
      const QString variable = name->text().trimmed();
      bool unique = identifier(variable);
      const bool sameBinding = !adding &&
          variable == entries[row].toObject().value("name").toString();
      for (int index = 0; index < entries.size(); ++index)
        if (index != row && entries[index].toObject().value("name").toString() == variable) unique = false;
      for (const auto &entry : variables)
        if (!sameBinding && text(entry.name) == variable) unique = false;
      for (const auto &entry : catalog) if (text(entry.name) == variable) unique = false;
      for (const auto &entry : customFunctions)
        if (!sameBinding && text(entry.name) == variable) unique = false;
      const bool changedMinimum = low->value() != displayedMinimum;
      const bool changedMaximum = high->value() != displayedMaximum;
      const double minimum = changedMinimum ? low->value()
          : control.value("min").toDouble(-1000000.);
      const double maximum = changedMaximum ? high->value()
          : control.value("max").toDouble(1000000.);
      if (!unique || minimum > maximum) {
        error->setText(SeExprEditor::tr("Use a unique variable identifier and an ordered numeric range.")); return;
      }
      const QString type = kind->currentText();
      QJsonValue parsed;
      if (type == "string") parsed = value->text();
      else if (type == "vector" || type == "color") {
        const auto array = QJsonDocument::fromJson(value->text().toUtf8()).array();
        bool valid = array.size() == 3;
        for (const auto &entry : array) valid = valid && entry.isDouble() && std::isfinite(entry.toDouble());
        if (!valid) { error->setText(SeExprEditor::tr("Use a finite three-component value such as [1,0.5,0].")); return; }
        parsed = array;
      } else {
        bool ok = false; const double number = value->text().toDouble(&ok);
        if ((type == "scalar") && (!ok || !std::isfinite(number))) {
          error->setText(SeExprEditor::tr("Use a finite numeric value.")); return;
        }
        parsed = (type == "scalar") ? QJsonValue(number) : QJsonValue(0.);
      }
      control["name"] = variable; control["label"] = label->text(); control["type"] = type;
      control["value"] = parsed;
      if (type == "animcurve") {
        if (!control.contains("keys")) {
          const auto defaults = SeExprControls::defaultAnimationCurve();
          for (auto it = defaults.begin(); it != defaults.end(); ++it)
            if (!control.contains(it.key())) control[it.key()] = it.value();
        }
        QString message;
        if (!SeExprControls::validateAnimationCurve(control, &message)) {
          error->setText(message); return;
        }
      }
      // Opening the display fields can round or clamp imported limits. Keep
      // the original JSON (including omitted limits) until that field changes.
      if (changedMinimum) control["min"] = minimum;
      if (changedMaximum) control["max"] = maximum;
      if (type == "string") control["annotationType"] = stringKind->currentData().toString();
      if (type == "curve" || type == "colorcurve") control["points"] = points;
      dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    if (!adding) {
      const QString oldName = entries[row].toObject().value("name").toString();
      rgb->setPlainText(SeExprEditor::renameVariable(rgb->toPlainText(), oldName, name->text().trimmed()));
      alpha->setPlainText(SeExprEditor::renameVariable(alpha->toPlainText(), oldName, name->text().trimmed()));
      if (object.contains("displacement"))
        displacement->setPlainText(SeExprEditor::renameVariable(displacement->toPlainText(), oldName, name->text().trimmed()));
      if (object.contains("opacity"))
        opacity->setPlainText(SeExprEditor::renameVariable(opacity->toPlainText(), oldName, name->text().trimmed()));
      entries[row] = control;
    } else entries.append(control);
    object["controls"] = entries; refreshControls(); changed();
  }
};

SeExprEditor::SeExprEditor(QWidget *parent) : QWidget(parent), m_imp(new Imp(this)) {
  auto &d = *m_imp;
  setObjectName("seExprEditor");
  auto layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
  d.backendRow = new QWidget(this); auto backendLayout = new QHBoxLayout(d.backendRow);
  backendLayout->setContentsMargins(0, 0, 0, 0);
  backendLayout->addWidget(new QLabel(tr("Evaluation Backend"), d.backendRow));
  d.backend = new QComboBox(d.backendRow); d.backend->setObjectName("seExprBackend");
  d.backend->addItem(tr("Interpreter"), "interpreter");
  d.backend->addItem(TSeExpr::Program::backendAvailable(TSeExpr::Backend::LLVM)
                         ? tr("LLVM") : tr("LLVM (unavailable in this build)"), "llvm");
  backendLayout->addWidget(d.backend); backendLayout->addStretch(); layout->addWidget(d.backendRow);
  d.tabs = new QTabWidget(this);
  d.rgb = new CodeEdit(this); d.rgb->setObjectName("seExprRgb"); new Highlighter(d.rgb->document());
  d.alpha = new CodeEdit(this); d.alpha->setObjectName("seExprAlpha"); new Highlighter(d.alpha->document());
  d.displacement = new CodeEdit(this); d.displacement->setObjectName("seExprDisplacement");
  new Highlighter(d.displacement->document());
  d.opacity = new CodeEdit(this); d.opacity->setObjectName("seExprOpacity");
  new Highlighter(d.opacity->document());
  d.tabs->addTab(d.rgb, tr("RGB")); d.tabs->addTab(d.alpha, tr("Alpha"));
  auto controlsPage = new QWidget(this); auto controlsLayout = new QVBoxLayout(controlsPage);
  d.controls = new QTableWidget(0, 4, controlsPage); d.controls->setObjectName("seExprControls");
  d.controls->setHorizontalHeaderLabels({tr("Display Name"), tr("Variable"), tr("Type"), tr("Default")});
  d.controls->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  d.controls->setSelectionBehavior(QAbstractItemView::SelectRows);
  controlsLayout->addWidget(d.controls);
  auto controlButtons = new QHBoxLayout();
  auto add = new QPushButton(tr("Add"), controlsPage), edit = new QPushButton(tr("Edit"), controlsPage), remove = new QPushButton(tr("Archive"), controlsPage);
  edit->setObjectName("seExprControlEdit");
  controlButtons->addWidget(add); controlButtons->addWidget(edit); controlButtons->addWidget(remove); controlButtons->addStretch();
  controlsLayout->addLayout(controlButtons); d.tabs->addTab(controlsPage, tr("Controls"));
  auto advancedButtons = new QHBoxLayout();
  auto annotations = new QPushButton(tr("Import Annotations"), controlsPage);
  annotations->setObjectName("seExprImportAnnotations");
  auto swatches = new QPushButton(tr("Color Swatches"), controlsPage);
  auto spectrum = new QPushButton(tr("Water Spectrum"), controlsPage);
  auto animationCurve = new QPushButton(tr("Animation Curve"), controlsPage);
  animationCurve->setObjectName("seExprAddAnimationCurve");
  auto refreshCurve = new QPushButton(tr("Refresh Linked Curve"), controlsPage);
  refreshCurve->setObjectName("seExprRefreshAnimationCurve");
  advancedButtons->addWidget(annotations); advancedButtons->addWidget(swatches);
  advancedButtons->addWidget(spectrum); advancedButtons->addStretch();
  controlsLayout->addLayout(advancedButtons);
  auto curveButtons = new QHBoxLayout();
  curveButtons->addWidget(animationCurve); curveButtons->addWidget(refreshCurve);
  curveButtons->addStretch(); controlsLayout->addLayout(curveButtons);
  auto presetPage = new QWidget(this); auto presetLayout = new QVBoxLayout(presetPage);
  d.presetFilter = new QLineEdit(presetPage); d.presetFilter->setPlaceholderText(tr("Search presets"));
  d.presets = new QListWidget(presetPage); d.presets->setObjectName("seExprPresets");
  presetLayout->addWidget(d.presetFilter); presetLayout->addWidget(d.presets);
  auto presetButtons = new QHBoxLayout();
  auto load = new QPushButton(tr("Load Draft"), presetPage), import = new QPushButton(tr("Import..."), presetPage),
       save = new QPushButton(tr("Save Preset..."), presetPage), exportButton = new QPushButton(tr("Export..."), presetPage);
  for (auto button : {load, import, save, exportButton}) presetButtons->addWidget(button);
  presetLayout->addLayout(presetButtons); d.tabs->addTab(presetPage, tr("Presets"));
  auto functionPage = new QWidget(this); auto functionLayout = new QVBoxLayout(functionPage);
  d.functionFilter = new QLineEdit(functionPage); d.functionFilter->setPlaceholderText(tr("Search functions"));
  d.functions = new QListWidget(functionPage); d.help = new QTextBrowser(functionPage);
  d.help->setOpenExternalLinks(false);
  functionLayout->addWidget(d.functionFilter); functionLayout->addWidget(d.functions); functionLayout->addWidget(d.help);
  d.tabs->addTab(functionPage, tr("Functions")); layout->addWidget(d.tabs);
  d.tabs->addTab(d.displacement, tr("Displacement"));
  d.tabs->setTabVisible(d.tabs->indexOf(d.displacement), false);
  d.tabs->addTab(d.opacity, tr("Opacity RGB"));
  d.tabs->setTabVisible(d.tabs->indexOf(d.opacity), false);
  d.status = new QLabel(this); d.status->setObjectName("seExprStatus"); d.status->setWordWrap(true); d.status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  d.preview = new QLabel(this); d.preview->setObjectName("seExprValuePreview"); d.preview->setWordWrap(true);
  d.status->setTextFormat(Qt::PlainText);
  d.preview->setTextFormat(Qt::PlainText);
  layout->addWidget(d.status); layout->addWidget(d.preview);
  auto buttons = new QHBoxLayout();
  auto validate = new QPushButton(tr("Validate"), this), revert = new QPushButton(tr("Revert"), this);
  auto range = new QPushButton(tr("Range Preview"), this);
  range->setObjectName("seExprRangePreview");
  auto insertCurve = new QPushButton(tr("Insert Animation Curve"), this);
  insertCurve->setObjectName("seExprInsertInlineCurve");
  auto editInlineCurve = new QPushButton(tr("Edit Inline Curve"), this);
  editInlineCurve->setObjectName("seExprEditInlineCurve");
  auto inlineButtons = new QHBoxLayout();
  inlineButtons->addWidget(insertCurve); inlineButtons->addWidget(editInlineCurve);
  inlineButtons->addStretch(); layout->addLayout(inlineButtons);
  d.apply = new QPushButton(tr("Apply"), this); d.apply->setObjectName("seExprApply");
  buttons->addWidget(validate); buttons->addWidget(revert); buttons->addWidget(range);
  buttons->addStretch(); buttons->addWidget(d.apply); layout->addLayout(buttons);
  d.debounce = new QTimer(this); d.debounce->setSingleShot(true); d.debounce->setInterval(300);
  QObject::connect(d.rgb, &QPlainTextEdit::textChanged, this, [this]() { m_imp->changed(); });
  QObject::connect(d.backend, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { m_imp->changed(); });
  QObject::connect(d.alpha, &QPlainTextEdit::textChanged, this, [this]() { m_imp->changed(); });
  QObject::connect(d.displacement, &QPlainTextEdit::textChanged, this, [this]() { m_imp->changed(); });
  QObject::connect(d.opacity, &QPlainTextEdit::textChanged, this, [this]() { m_imp->changed(); });
  QObject::connect(d.debounce, &QTimer::timeout, this, [this]() { validateDraft(); });
  QObject::connect(validate, &QPushButton::clicked, this, [this]() { validateDraft(); });
  QObject::connect(d.apply, &QPushButton::clicked, this, [this]() { applyDraft(); });
  QObject::connect(revert, &QPushButton::clicked, this, &SeExprEditor::revertDraft);
  QObject::connect(range, &QPushButton::clicked, this, [this]() {
    if (!validateDraft()) return;
    auto &d = *m_imp;
    const QString draft = d.documentMode ? document() : source();
    RangeEvaluator evaluator;
    if (d.rangeFactory) evaluator = d.rangeFactory(draft);
    else {
      auto variables = d.variables;
      if (d.documentMode) {
        for (const auto &entry : d.object.value("controls").toArray()) {
          const auto control = entry.toObject();
          const QString kind = control.value("type").toString();
          if (kind == "curve" || kind == "colorcurve" || kind == "animcurve") continue;
          const auto value = control.value("value");
          TSeExpr::Value bound;
          if (value.isString()) bound = TSeExpr::Value::string(bytes(value.toString()));
          else if (value.isArray()) {
            const auto components = value.toArray();
            bound = TSeExpr::Value::vector(components[0].toDouble(), components[1].toDouble(),
                                         components[2].toDouble());
          } else bound = TSeExpr::Value::scalar(value.toDouble());
          variables.push_back({bytes(control.value("name").toString()), bound});
        }
      }
      const auto functions = d.customFunctions;
      const QString formula = d.rgb->toPlainText();
      TSeExpr::Options options;
      options.resultType = d.type;
      options.backend = d.backend->currentData().toString() == "llvm"
                            ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
      evaluator = [variables, functions, formula, options](
                      double frame, std::vector<double> &values, QString &error) mutable {
        double fps = 24.0;
        for (const auto &variable : variables)
          if (variable.name == "fps" && !variable.value.number.empty())
            fps = variable.value.number[0];
        if (!std::isfinite(fps) || fps <= 0) {
          error = tr("Range preview requires a positive finite frame rate.");
          return false;
        }
        for (auto &variable : variables) {
          if (variable.name == "frame") variable.value = TSeExpr::Value::scalar(frame);
          else if (variable.name == "time") variable.value = TSeExpr::Value::scalar((frame - 1.0) / fps);
        }
        TSeExpr::Program program;
        TSeExpr::Value result;
        if (!program.compile(bytes(formula), variables, functions, options) ||
            !program.evaluate(result)) {
          error = text(program.errorMessage());
          return false;
        }
        if (result.type == TSeExpr::Type::String) {
          error = tr("Range preview requires numeric output.");
          return false;
        }
        values = std::move(result.number);
        return true;
      };
    }
    if (!evaluator) {
      d.status->setText(tr("The current host has no range preview evaluator."));
      return;
    }
    SeExprRangeViewer::show(this, evaluator, 1.0, 48.0, 1.0);
  });
  QObject::connect(add, &QPushButton::clicked, this, [this]() { m_imp->editControl(-1); });
  QObject::connect(edit, &QPushButton::clicked, this, [this]() { if (m_imp->controls->currentRow() >= 0) m_imp->editControl(m_imp->controls->currentRow()); });
  QObject::connect(d.controls, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { m_imp->editControl(row); });
  QObject::connect(remove, &QPushButton::clicked, this, [this]() {
    auto &d = *m_imp; const int row = d.controls->currentRow(); if (row < 0) return;
    auto entries = d.object.value("controls").toArray();
    auto archive = d.object.value("archive").toArray(); archive.append(entries[row]); entries.removeAt(row);
    d.object["archive"] = archive; d.object["controls"] = entries; d.refreshControls(); d.changed();
  });
  QObject::connect(annotations, &QPushButton::clicked, this, [this]() {
    auto &d = *m_imp;
    QJsonArray controls = d.object.value("controls").toArray();
    QStringList diagnostics;
    for (auto editor : {d.rgb, d.alpha, d.displacement, d.opacity}) {
      if (editor == d.displacement && !d.object.contains("displacement")) continue;
      if (editor == d.opacity && !d.object.contains("opacity")) continue;
      const auto imported = SeExprControls::importAnnotations(editor->toPlainText(), controls,
          editor == d.rgb ? "rgb" : editor == d.alpha ? "alpha" :
          editor == d.displacement ? "displacement" : "opacity");
      editor->setPlainText(imported.source);
      for (const auto &control : imported.controls) {
        const QString id = control.toObject().value("id").toString();
        bool found = false;
        for (int index = 0; index < controls.size(); ++index) {
          if (controls[index].toObject().value("id").toString() == id) {
            controls[index] = control; found = true; break;
          }
        }
        if (!found) controls.append(control);
      }
      diagnostics.append(imported.diagnostics);
    }
    d.object["controls"] = controls; d.refreshControls(); d.changed();
    if (!diagnostics.isEmpty()) d.status->setText(diagnostics.join('\n'));
  });
  const auto addAdvanced = [this](bool water) {
    auto &d = *m_imp;
    auto controls = d.object.value("controls").toArray();
    const QString name = QString(water ? "spectrum%1" : "swatches%1").arg(controls.size() + 1);
    QJsonObject control{{"id", "control_" + QUuid::createUuid().toString(QUuid::Id128)},
                        {"name", name}, {"label", name},
                        {"type", water ? "curve" : "colorcurve"},
                        {"min", 0.}, {"max", 1.}, {"value", 0.}};
    if (!(water ? SeExprControls::editSpectrum(this, control)
                : SeExprControls::editSwatches(this, control))) return;
    controls.append(control); d.object["controls"] = controls;
    d.refreshControls(); d.changed();
  };
  QObject::connect(swatches, &QPushButton::clicked, this, [addAdvanced]() { addAdvanced(false); });
  QObject::connect(spectrum, &QPushButton::clicked, this, [addAdvanced]() { addAdvanced(true); });
  QObject::connect(animationCurve, &QPushButton::clicked, this, [this]() {
    auto &d = *m_imp;
    auto entries = d.object.value("controls").toArray();
    auto control = SeExprControls::defaultAnimationCurve();
    const QString name = "animation_" + QUuid::createUuid().toString(QUuid::Id128);
    control["id"] = "control_" + QUuid::createUuid().toString(QUuid::Id128);
    control["name"] = name; control["label"] = tr("Animation Curve");
    if (!SeExprControls::editAnimationCurve(this, control, d.curveLinkResolver)) return;
    entries.append(control); d.object["controls"] = entries;
    d.refreshControls(); d.controls->selectRow(entries.size() - 1); d.changed();
  });
  QObject::connect(refreshCurve, &QPushButton::clicked, this, [this]() {
    auto &d = *m_imp;
    const int row = d.controls->currentRow();
    auto entries = d.object.value("controls").toArray();
    if (row < 0 || row >= entries.size() ||
        entries[row].toObject().value("type").toString() != "animcurve") {
      d.status->setText(tr("Select an animation curve to refresh.")); return;
    }
    auto control = entries[row].toObject();
    const QString link = control.value("link").toString().trimmed();
    if (link.isEmpty() || !d.curveLinkResolver) {
      d.status->setText(tr("This curve needs a linked native channel and an available host context."));
      return;
    }
    QString error;
    bool refreshed = false;
    try {
      refreshed = d.curveLinkResolver(link, control, error) &&
                  SeExprControls::validateAnimationCurve(control, &error);
    } catch (...) {
      error = tr("The linked native channel could not be read.");
    }
    if (!refreshed) {
      d.status->setText(error.isEmpty() ? tr("The linked curve could not be refreshed.") : error);
      return;
    }
    entries[row] = control; d.object["controls"] = entries;
    d.refreshControls(); d.controls->selectRow(row); d.changed();
    d.status->setText(tr("Linked curve refreshed in the draft. Apply commits the snapshot."));
  });
  const auto inlineCurveAction = [this](bool insert) {
    auto &d = *m_imp;
    auto *edit = dynamic_cast<CodeEdit *>(d.tabs->currentWidget());
    if (!edit) edit = d.rgb;
    QString formula = insert ? QString() : edit->toPlainText();
    QString error;
    if (!SeExprControls::editInlineAnimationCurve(this, formula, &error, insert,
                                                 d.curveLinkResolver)) {
      if (!error.isEmpty()) d.status->setText(error);
      return;
    }
    if (insert) {
      auto cursor = edit->textCursor(); cursor.insertText(formula);
      edit->setTextCursor(cursor);
    } else edit->setPlainText(formula);
  };
  QObject::connect(insertCurve, &QPushButton::clicked, this, [inlineCurveAction]() { inlineCurveAction(true); });
  QObject::connect(editInlineCurve, &QPushButton::clicked, this, [inlineCurveAction]() { inlineCurveAction(false); });
  QObject::connect(d.presetFilter, &QLineEdit::textChanged, this, [this]() { m_imp->refreshPresets(); });
  QObject::connect(d.functionFilter, &QLineEdit::textChanged, this, [this]() { m_imp->refreshFunctions(); });
  QObject::connect(d.functions, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
    m_imp->help->setPlainText(item ? item->data(Qt::UserRole).toString() : QString());
  });
  const auto loadSelected = [this]() {
    auto item = m_imp->presets->currentItem(); if (!item) return;
    QString error; if (!importPreset(item->data(Qt::UserRole).toString().toUtf8(), &error)) m_imp->status->setText(error);
  };
  QObject::connect(load, &QPushButton::clicked, this, loadSelected);
  QObject::connect(d.presets, &QListWidget::itemDoubleClicked, this, [loadSelected](QListWidgetItem *) { loadSelected(); });
  QObject::connect(import, &QPushButton::clicked, this, [this]() {
    const QString name = QFileDialog::getOpenFileName(this, tr("Import Expression Preset"), QString(), tr("Expression presets (*.json)"));
    if (name.isEmpty()) return; QFile file(name); QString error;
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxPresetBytes || !importPreset(file.readAll(), &error))
      m_imp->status->setText(error.isEmpty() ? tr("The preset could not be read or is too large.") : error);
  });
  QObject::connect(exportButton, &QPushButton::clicked, this, [this]() {
    const QString name = QFileDialog::getSaveFileName(this, tr("Export Expression Preset"), QString(), tr("Expression presets (*.json)"));
    if (name.isEmpty()) return; QSaveFile file(name);
    const QByteArray data = exportPreset();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
      m_imp->status->setText(tr("The expression preset could not be saved."));
  });
  QObject::connect(save, &QPushButton::clicked, this, [this]() {
    auto &d = *m_imp;
    if (d.presetDirectory.isEmpty()) { d.status->setText(tr("The host has not configured a user preset folder. Use Export to save a portable preset.")); return; }
    bool ok = false; const QString title = QInputDialog::getText(this, tr("Save Expression Preset"), tr("Name"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || title.isEmpty()) return;
    QString stem = title; stem.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
    if (stem.isEmpty()) stem = "preset";
    QDir directory(d.presetDirectory);
    if (!directory.exists() && !directory.mkpath(".")) { d.status->setText(tr("The preset folder could not be created.")); return; }
    const QString path = directory.filePath(stem + ".json");
    if (QFileInfo::exists(path) && QMessageBox::question(this, tr("Replace Preset"), tr("A preset with this name exists. Replace it?")) != QMessageBox::Yes) return;
    QJsonObject preset = QJsonDocument::fromJson(exportPreset()).object(); preset["title"] = title;
    const QByteArray data = QJsonDocument(preset).toJson(); QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { d.status->setText(tr("The preset could not be saved.")); return; }
    d.refreshPresets();
  });
  d.catalog = TSeExpr::Program::builtinFunctions();
  const auto nativeFunctions = TSeExpr::Program::nativeFunctions();
  d.catalog.insert(d.catalog.end(), nativeFunctions.begin(), nativeFunctions.end());
  d.variables = {{"u", TSeExpr::Value::scalar(.5)}, {"v", TSeExpr::Value::scalar(.5)},
    {"x", TSeExpr::Value::scalar(0)}, {"y", TSeExpr::Value::scalar(0)}, {"frame", TSeExpr::Value::scalar(1)},
    {"time", TSeExpr::Value::scalar(0)}, {"fps", TSeExpr::Value::scalar(24)},
    {"seed", TSeExpr::Value::scalar(0)}, {"src", TSeExpr::Value::vector(0, 0, 0)},
    {"r", TSeExpr::Value::scalar(0)}, {"g", TSeExpr::Value::scalar(0)},
    {"b", TSeExpr::Value::scalar(0)}, {"a", TSeExpr::Value::scalar(1)},
    {"width", TSeExpr::Value::scalar(1)}, {"height", TSeExpr::Value::scalar(1)}};
  d.loadObject(defaultDocument()); d.committed = compact(d.object);
  d.refreshFunctions(); d.refreshPresets(); d.apply->setEnabled(false);
}

SeExprEditor::~SeExprEditor() = default;

void SeExprEditor::setDocument(const QString &value, bool discardDraft) {
  auto &d = *m_imp;
  QJsonParseError parse; const auto document = QJsonDocument::fromJson(value.toUtf8(), &parse);
  QJsonObject object = document.isObject() ? document.object() : defaultDocument();
  if (!object.contains("backend")) object["backend"] = "interpreter";
  if (object.contains("displacement") && !object.contains("opacity"))
    object["opacity"] = "[1,1,1]";
  if (!document.isObject()) object["rgb"] = value;
  const QString canonical = compact(object);
  if (d.documentMode && canonical == d.committed && !discardDraft) return;
  const bool preserve = isModified() && !discardDraft;
  d.committed = canonical; d.documentMode = true;
  d.backendRow->show();
  d.tabs->setTabText(0, tr("RGB")); d.tabs->setTabVisible(1, true); d.tabs->setTabVisible(2, true);
  if (!preserve) d.loadObject(object);
  d.refreshPresets();
  if (!document.isObject()) d.status->setText(tr("The stored document is invalid. Its original text is retained in the RGB draft for repair."));
}
QString SeExprEditor::document() const { return compact(m_imp->currentObject()); }
void SeExprEditor::setSource(const QString &value, bool discardDraft) {
  auto &d = *m_imp;
  if (!d.documentMode && d.committed == value && !discardDraft) return;
  const bool preserve = isModified() && !discardDraft;
  d.documentMode = false; d.committed = value;
  { const QSignalBlocker blocker(d.backend); d.backend->setCurrentIndex(0); }
  d.backendRow->hide();
  d.tabs->setTabText(0, tr("Expression")); d.tabs->setTabVisible(1, false); d.tabs->setTabVisible(2, false);
  d.tabs->setTabVisible(d.tabs->indexOf(d.displacement), false);
  d.tabs->setTabVisible(d.tabs->indexOf(d.opacity), false);
  if (!preserve) { d.loading = true; d.rgb->setPlainText(value); d.loading = false; }
  d.apply->setEnabled(isModified());
  d.refreshPresets();
}
QString SeExprEditor::source() const { return m_imp->rgb->toPlainText(); }
void SeExprEditor::setResultType(TSeExpr::Type type) { m_imp->type = type; m_imp->refreshPresets(); }
void SeExprEditor::setSurfaceDocumentSupported(bool supported) {
  auto &d = *m_imp;
  d.surfaceSupported = supported;
  d.tabs->setTabVisible(d.tabs->indexOf(d.displacement),
                       supported && d.documentMode && d.object.contains("displacement"));
  d.tabs->setTabVisible(d.tabs->indexOf(d.opacity),
                       supported && d.documentMode && d.object.contains("opacity"));
}
void SeExprEditor::setContext(const std::vector<TSeExpr::Variable> &variables,
                             const std::vector<TSeExpr::Function> &functions) {
  m_imp->variables = variables; m_imp->customFunctions = functions; m_imp->refreshFunctions();
}
void SeExprEditor::setFrame(double frame) { m_imp->frame = frame; }
void SeExprEditor::setPresetDirectory(const QString &directory) {
  m_imp->presetDirectory = directory; m_imp->refreshPresets();
}
void SeExprEditor::setBuiltinPresetDirectory(const QString &directory) {
  m_imp->builtinPresetDirectory = directory; m_imp->refreshPresets();
}
void SeExprEditor::setValidator(std::function<QString(const QString &)> validator) { m_imp->validator = std::move(validator); }
void SeExprEditor::setPreviewEvaluator(std::function<QString(const QString &)> evaluator) {
  m_imp->evaluator = std::move(evaluator);
  m_imp->checkedEvaluator = {};
}
void SeExprEditor::setCheckedPreviewEvaluator(
    std::function<bool(const QString &, QString &, QString &)> evaluator) {
  m_imp->checkedEvaluator = std::move(evaluator);
  m_imp->evaluator = {};
}
void SeExprEditor::setRangeEvaluator(
    std::function<bool(const QString &, double, std::vector<double> &, QString &)> evaluator) {
  m_imp->rangeFactory = [evaluator](const QString &source) -> RangeEvaluator {
    return [source, evaluator](double frame, std::vector<double> &values, QString &error) {
      return evaluator && evaluator(source, frame, values, error);
    };
  };
}
void SeExprEditor::setRangeEvaluatorFactory(
    std::function<RangeEvaluator(const QString &)> factory) {
  m_imp->rangeFactory = std::move(factory);
}
void SeExprEditor::setAnimationCurveLinkResolver(AnimationCurveLinkResolver resolver) {
  m_imp->curveLinkResolver = std::move(resolver);
}
bool SeExprEditor::isModified() const {
  return (m_imp->documentMode ? document() : source()) != m_imp->committed;
}

bool SeExprEditor::validateDraft() {
  auto &d = *m_imp; d.debounce->stop();
  for (auto *edit : {d.rgb, d.alpha, d.displacement, d.opacity}) edit->setExtraSelections({});
  if (d.documentMode && !d.surfaceSupported &&
      (d.object.contains("displacement") || d.object.contains("opacity"))) {
    d.status->setText(tr("This preset requires an Expression Surface geometry host. Load an image preset for the current effect."));
    d.preview->clear(); return false;
  }
  if (d.documentMode && d.object.value("version").toInt(-1) != 1) {
    d.status->setText(tr("The expression document version is unsupported. Its source is preserved."));
    d.preview->clear();
    return false;
  }
  if (d.documentMode &&
      ((d.object.contains("engine") &&
        d.object.value("engine").toString() != text(TSeExpr::Program::engineVersion())) ||
       (d.object.contains("functionPolicy") &&
        d.object.value("functionPolicy").toInt(-1) !=
            int(TSeExpr::Program::functionPolicyVersion())))) {
    d.status->setText(tr("The saved expression engine or function policy is unsupported. Its source is preserved."));
    d.preview->clear();
    return false;
  }
  if (d.documentMode && d.backend->currentData().toString() != "interpreter" &&
      d.backend->currentData().toString() != "llvm") {
    d.status->setText(tr("The expression backend is unsupported. Select an available backend to continue."));
    d.preview->clear();
    return false;
  }
  if (d.validator) {
    const QString error = d.validator(d.documentMode ? document() : source());
    if (!error.isEmpty()) { d.status->setText(error); d.preview->clear(); return false; }
  }
  // Host document validation handles curves, source sampling and native schema.
  // A host preview callback supplies its actual current-frame value.
  if (d.checkedEvaluator) {
    QString value, error;
    if (!d.checkedEvaluator(d.documentMode ? document() : source(), value, error)) {
      d.status->setText(error.isEmpty() ? tr("The formula could not be evaluated.") : error);
      d.preview->clear();
      return false;
    }
    d.preview->setText(value);
    d.status->setText(tr("Formula validated. Apply commits one undoable edit."));
    return true;
  }
  if (d.evaluator) {
    d.preview->setText(d.evaluator(d.documentMode ? document() : source()));
    d.status->setText(tr("Formula validated. Apply commits one undoable edit."));
    return true;
  }
  auto variables = d.variables;
  if (d.documentMode) {
    for (const auto &entry : d.object.value("controls").toArray()) {
      const auto control = entry.toObject(); const auto value = control.value("value");
      const QString type = control.value("type").toString();
      if (type == "curve" || type == "colorcurve" || type == "animcurve") continue;
      TSeExpr::Value native;
      if (type == "string") native = TSeExpr::Value::string(bytes(value.toString()));
      else if (value.isArray()) { const auto a = value.toArray(); native = TSeExpr::Value::vector(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()); }
      else native = TSeExpr::Value::scalar(value.toDouble());
      variables.push_back({bytes(control.value("name").toString()), native});
    }
  }
  QStringList results;
  std::vector<std::pair<CodeEdit *, TSeExpr::Type>> fields{{d.rgb, d.type}};
  if (d.documentMode) {
    fields.push_back({d.alpha, TSeExpr::Type::Scalar});
    if (d.object.contains("displacement")) fields.push_back({d.displacement, TSeExpr::Type::Scalar});
    if (d.object.contains("opacity")) fields.push_back({d.opacity, TSeExpr::Type::Vector});
  }
  for (const auto &field : fields) {
    auto edit = field.first;
    TSeExpr::Options options; options.resultType = field.second;
    options.backend = d.backend->currentData().toString() == "llvm" ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
    TSeExpr::Program program;
    if (!program.compile(bytes(edit->toPlainText()), variables, d.customFunctions, options)) {
      d.status->setText(text(program.errorMessage())); d.preview->clear();
      QList<QTextEdit::ExtraSelection> selections;
      for (const auto &error : program.errors()) {
        const QByteArray utf8 = edit->toPlainText().toUtf8();
        const int begin = QString::fromUtf8(utf8.left(std::clamp(error.begin, 0, int(utf8.size())))).size();
        const int end = QString::fromUtf8(utf8.left(std::clamp(error.end, 0, int(utf8.size())))).size();
        QTextEdit::ExtraSelection selection; selection.cursor = edit->textCursor();
        selection.cursor.setPosition(begin); selection.cursor.setPosition(std::min(int(edit->toPlainText().size()), std::max(begin + 1, end)), QTextCursor::KeepAnchor);
        selection.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        selection.format.setUnderlineColor(QColor(220, 60, 60)); selections.append(selection);
      }
      edit->setExtraSelections(selections); return false;
    }
    TSeExpr::Value result;
    if (!program.evaluate(result)) { d.status->setText(text(program.errorMessage())); d.preview->clear(); return false; }
    if (result.type == TSeExpr::Type::String) results << text(result.text);
    else {
      QStringList numbers; const int count = result.type == TSeExpr::Type::Scalar ? 1 : int(result.number.size());
      for (int component = 0; component < count; ++component) numbers << QString::number(result.number[component], 'g', 12);
      results << numbers.join(", ");
    }
  }
  d.status->setText(tr("Valid. Apply commits one undoable edit."));
  d.preview->setText(d.documentMode ? tr("Sample at u=0.5, v=0.5 — RGB: %1; Alpha: %2").arg(results[0], results[1])
                                  : tr("Current value: %1").arg(results[0]));
  for (std::size_t index = 2; index < fields.size(); ++index)
    d.preview->setText(d.preview->text() +
        (fields[index].first == d.displacement ? tr("; Displacement: %1") : tr("; Opacity RGB: %1"))
            .arg(results[int(index)]));
  return true;
}
bool SeExprEditor::applyDraft() {
  if (!isModified()) return true;
  if (!validateDraft()) return false;
  auto &d = *m_imp;
  const QString applied = d.documentMode ? document() : source();
  d.committed = applied;
  if (d.documentMode) { d.object = d.currentObject(); emit documentApplied(applied); }
  else emit sourceApplied(applied);
  d.apply->setEnabled(false); emit modifiedChanged(false); return true;
}
void SeExprEditor::revertDraft() {
  auto &d = *m_imp;
  if (d.documentMode) setDocument(d.committed, true); else setSource(d.committed, true);
  d.preview->clear(); d.status->setText(tr("Draft reverted."));
}
bool SeExprEditor::importPreset(const QByteArray &data, QString *error) {
  const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
  if (data.size() > MaxPresetBytes) return fail(tr("The preset exceeds the size limit."));
  const auto document = QJsonDocument::fromJson(data);
  if (!document.isObject()) return fail(tr("The preset is not a JSON object."));
  QJsonObject value = document.object();
  if (value.contains("format")) {
    if (value.value("format").toString() != "groundzero-seexpr" || value.value("version").toInt() != 1)
      return fail(tr("The preset envelope has an unsupported format or version."));
    value = value.value("document").toObject();
  }
  if (value.value("version").toInt() != 1 || !value.value("rgb").isString() ||
      !value.value("alpha").isString() || !value.value("controls").isArray() ||
      (value.contains("displacement") && !value.value("displacement").isString()) ||
      (value.contains("opacity") && !value.value("opacity").isString()))
    return fail(tr("The preset has an unsupported version or incomplete expression document."));
  if (m_imp->documentMode && !m_imp->surfaceSupported &&
      (value.contains("displacement") || value.contains("opacity")))
    return fail(tr("This preset requires an Expression Surface geometry host."));
  const auto controls = value.value("controls").toArray();
  if (controls.size() > 512) return fail(tr("The preset has too many controls."));
  std::set<QString> identifiers, names;
  for (const auto &entry : controls) {
    const auto control = entry.toObject();
    if (!entry.isObject() || !validControlShape(control) ||
        !identifiers.insert(control.value("id").toString()).second ||
        !names.insert(control.value("name").toString()).second)
      return fail(tr("The preset contains an invalid or duplicated control."));
    QJsonArray previous = m_imp->object.value("controls").toArray();
    for (const auto &entry : m_imp->object.value("archive").toArray()) previous.append(entry);
    for (const auto &old : previous) {
      const auto oldControl = old.toObject();
      if (oldControl.value("id") == control.value("id") &&
          oldControl.value("type") != control.value("type"))
        return fail(tr("A preset cannot change the type of an existing control identifier."));
    }
  }
  if (!m_imp->documentMode) {
    m_imp->rgb->setPlainText(value.value("rgb").toString());
  } else {
    // Preserve removed controls for native curve/undo restoration.
    const auto next = value.value("controls").toArray();
    std::set<QString> active;
    for (const auto &entry : next) active.insert(entry.toObject().value("id").toString());
    std::map<QString, QJsonObject> retained;
    for (const auto &list : {m_imp->object.value("archive").toArray(),
                            m_imp->object.value("controls").toArray(),
                            value.value("archive").toArray()}) {
      for (const auto &entry : list) {
        const auto control = entry.toObject();
        const QString id = control.value("id").toString();
        if (!active.count(id)) retained[id] = control;
      }
    }
    QJsonArray archive;
    for (const auto &entry : retained) archive.append(entry.second);
    value["archive"] = archive;
    // Image presets can supply color/controls to a surface host without
    // removing its editable geometry source.
    if (m_imp->surfaceSupported && m_imp->object.contains("displacement") && !value.contains("displacement"))
      value["displacement"] = m_imp->displacement->toPlainText();
    if (m_imp->surfaceSupported && m_imp->object.contains("opacity") && !value.contains("opacity"))
      value["opacity"] = m_imp->opacity->toPlainText();
    m_imp->loadObject(value); m_imp->changed();
  }
  if (error) error->clear(); return true;
}
QByteArray SeExprEditor::exportPreset() const {
  QJsonObject value = m_imp->currentObject();
  if (!m_imp->documentMode) value["rgb"] = source();
  return QJsonDocument(QJsonObject{{"format", "groundzero-seexpr"}, {"version", 1},
                                   {"document", value}}).toJson(QJsonDocument::Indented);
}
QString SeExprEditor::renameVariable(const QString &source, const QString &oldName,
                                    const QString &newName) {
  if (!identifier(oldName) || !identifier(newName) || oldName == newName) return source;
  QString result; bool quoted = false, comment = false, escaped = false;
  QChar quote;
  for (int position = 0; position < source.size();) {
    const QChar c = source[position];
    if (comment) { result += c; ++position; if (c == '\n') comment = false; continue; }
    if (quoted) {
      result += c; ++position;
      if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == quote) quoted = false;
      continue;
    }
    if (c == '#') { comment = true; result += c; ++position; continue; }
    if (c == '"' || c == '\'') { quoted = true; quote = c; result += c; ++position; continue; }
    if (c.isDigit() || (c == '.' && position + 1 < source.size() &&
                        source[position + 1].isDigit())) {
      // The exponent of a literal such as 2e5 is not the variable e5.
      int end = position + 1;
      while (end < source.size()) {
        const QChar d = source[end];
        if (d.isDigit() || d == '.')
          ++end;
        else if (d == 'e' || d == 'E') {
          ++end;
          if (end < source.size() && (source[end] == '+' || source[end] == '-'))
            ++end;
        } else
          break;
      }
      result += source.mid(position, end - position);
      position = end;
      continue;
    }
    if (c.isLetter() || c == '_') {
      int end = position + 1;
      while (end < source.size() && (source[end].isLetterOrNumber() || source[end] == '_')) ++end;
      if (source.mid(end, 2) == "::") {
        end += 2;
        while (end < source.size() && (source[end].isLetterOrNumber() || source[end] == '_')) ++end;
      }
      const QString name = source.mid(position, end - position);
      result += name == oldName ? newName : name; position = end;
    } else { result += c; ++position; }
  }
  return result;
}
void SeExprEditor::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::PaletteChange)
    for (auto edit : {m_imp->rgb, m_imp->alpha, m_imp->displacement, m_imp->opacity})
      for (auto highlighter : edit->document()->findChildren<QSyntaxHighlighter *>()) highlighter->rehighlight();
}
