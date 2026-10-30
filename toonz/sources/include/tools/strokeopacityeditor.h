#pragma once

#include "tools/vectorfillreview.h"
#include "tstroke.h"
#include "tstrokeopacity.h"
#include <QAbstractTableModel>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <functional>
#include <set>

namespace StrokeOpacityEditor {
class Model final : public QAbstractTableModel {
  StrokeOpacity::Profile m_profile;
public:
  explicit Model(QObject *parent) : QAbstractTableModel(parent) {}
  const StrokeOpacity::Profile &profile() const { return m_profile; }
  void setProfile(const StrokeOpacity::Profile &value) {
    beginResetModel(); m_profile = value; endResetModel();
  }
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : int(m_profile.size());
  }
  int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 2; }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() >= rowCount() ||
        (role != Qt::DisplayRole && role != Qt::EditRole)) return {};
    const auto &p = m_profile[index.row()];
    const double value = index.column() == 0 ? p.x : p.y;
    return role == Qt::EditRole ? QVariant(value) : QVariant(QString::number(value, 'g', 17));
  }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;
    return section == 0 ? tr("Length (0–1)") : tr("Opacity (0–1)");
  }
  Qt::ItemFlags flags(const QModelIndex &index) const override {
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable : Qt::NoItemFlags;
  }
  bool setData(const QModelIndex &index, const QVariant &value, int role) override {
    if (role != Qt::EditRole || !index.isValid() || index.row() >= rowCount()) return false;
    bool ok; const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number) || number < 0. || number > 1.) return false;
    double &old = index.column() == 0 ? m_profile[index.row()].x : m_profile[index.row()].y;
    if (old == number) return false;
    old = number;
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
  }
};

class NumberDelegate final : public QStyledItemDelegate {
public:
  explicit NumberDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                         const QModelIndex &) const override {
    auto *field = new QDoubleSpinBox(parent);
    field->setRange(0., 1.); field->setDecimals(17); field->setSingleStep(.01);
    return field;
  }
};

class Curve final : public QWidget {
  Model *m_model;
  int m_drag = -1;
  QRectF plot() const { return QRectF(30, 12, std::max(1, width()-44), std::max(1, height()-40)); }
  QPointF point(const TPointD &p) const {
    const auto r = plot(); return {r.left()+p.x*r.width(), r.bottom()-p.y*r.height()};
  }
public:
  std::function<void(int)> selectRow;
  Curve(Model *model, QWidget *parent) : QWidget(parent), m_model(model) {
    setMinimumSize(300, 180); setObjectName("strokeOpacityCurve");
    setToolTip(tr("Drag a knot to change its length and opacity. Use the table for exact values and steps."));
  }
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());
    const auto r = plot(); painter.setPen(palette().mid().color()); painter.drawRect(r);
    painter.setPen(palette().text().color());
    painter.drawText(QPointF(10, r.top()+5), "1"); painter.drawText(QPointF(10,r.bottom()), "0");
    painter.drawText(QPointF(r.left(),height()-8), tr("Start"));
    painter.drawText(QPointF(r.right()-26,height()-8), tr("End"));
    const auto &profile = m_model->profile();
    QPainterPath path;
    if (profile.empty()) { path.moveTo(r.topLeft()); path.lineTo(r.topRight()); }
    else {
      path.moveTo(point(profile.front()));
      for (size_t i = 1; i < profile.size(); ++i) path.lineTo(point(profile[i]));
    }
    painter.setPen(QPen(StrokeOpacity::valid(profile) ? palette().highlight().color() : QColor(Qt::red), 2));
    painter.drawPath(path);
    painter.setBrush(palette().highlight());
    for (const auto &p : profile) painter.drawEllipse(point(p), 3, 3);
  }
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton) return;
    double nearest = 81.; m_drag = -1;
    const auto &profile = m_model->profile();
    for (int i = 0; i < int(profile.size()); ++i) {
      const QPointF delta = point(profile[i]) - event->pos();
      const double distance = delta.x()*delta.x()+delta.y()*delta.y();
      if (distance <= nearest) { nearest = distance; m_drag = i; }
    }
    if (m_drag >= 0 && selectRow) selectRow(m_drag);
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (m_drag < 0 || m_drag >= m_model->rowCount() || !(event->buttons() & Qt::LeftButton)) return;
    const auto r = plot();
    const double y = std::clamp((r.bottom()-event->pos().y())/r.height(), 0., 1.);
    double x = std::clamp((event->pos().x()-r.left())/r.width(), 0., 1.);
    if (m_drag == 0) x = 0.;
    else if (m_drag == m_model->rowCount()-1) x = 1.;
    else {
      const double left = m_model->profile()[m_drag-1].x;
      const double right = m_model->profile()[m_drag+1].x;
      // The exact-value table may temporarily contain an unordered profile.
      // Keep vertical editing available without passing reversed bounds to clamp.
      x = left <= right ? std::clamp(x, left, right) : m_model->profile()[m_drag].x;
    }
    m_model->setData(m_model->index(m_drag,0), x, Qt::EditRole);
    m_model->setData(m_model->index(m_drag,1), y, Qt::EditRole);
  }
  void mouseReleaseEvent(QMouseEvent *) override { m_drag = -1; }
};

class Dialog final : public QDialog {
  VectorModalGuard m_owner;
  Model *m_model;
  QTableView *m_table;
  Curve *m_curve;
  QLabel *m_status;
  QPushButton *m_apply;
  VectorFillReview::Swatch *m_preview;
  TVectorImageP m_source;
  std::set<int> m_selection;
  bool m_mixed = false, m_changed = false;
  QTimer *m_previewTimer;
  void updateState() {
    const bool valid = StrokeOpacity::valid(m_model->profile());
    m_apply->setEnabled(valid && (!m_mixed || m_changed));
    m_status->setText(!valid ? tr("Use ordered lengths from 0 to 1, with an endpoint at each end. A step may have two knots at the same length.") :
        m_mixed && !m_changed ? tr("The selected strokes have different envelopes. Edit a knot or choose a preset to replace them together.") :
        tr("The envelope multiplies each stroke's own overall opacity. Cancel leaves all drawings unchanged."));
    m_curve->update();
    if (valid && (!m_mixed || m_changed)) m_previewTimer->start();
    else m_previewTimer->stop();
  }
  void preview() {
    if (!StrokeOpacity::valid(m_model->profile()) || (m_mixed && !m_changed)) return;
    TVectorImageP proposed = m_source->clone();
    for (int index : m_selection) proposed->getStroke(index)->setOpacityProfile(m_model->profile());
    m_preview->setImage(proposed, m_source->getBBox());
  }
  void insert(bool step) {
    auto profile = m_model->profile();
    if (profile.size() >= 256) {
      m_status->setText(tr("Existing dense envelopes are preserved. The editor can add knots up to 256; remove knots to add more.")); return;
    }
    if (profile.empty()) { m_model->setProfile({{0.,1.},{1.,1.}}); return; }
    if (profile.size() == 1) {
      const auto point = profile.front();
      if (point.x != 0.) profile.insert(profile.begin(), TPointD(0.,point.y));
      if (point.x != 1.) profile.emplace_back(1.,point.y);
      m_model->setProfile(profile);
      return;
    }
    int row = std::clamp(m_table->currentIndex().row(), 0, int(profile.size())-1);
    if (step) profile.insert(profile.begin()+row+1, profile[row]);
    else {
      row = std::min(row, int(profile.size())-2);
      const auto a = profile[row], b = profile[row+1];
      profile.insert(profile.begin()+row+1, TPointD((a.x+b.x)*.5, (a.y+b.y)*.5));
    }
    m_model->setProfile(profile); m_table->selectRow(row+1);
  }
public:
  Dialog(const TVectorImageP &image, const std::set<int> &selection, QWidget *parent = nullptr)
      : QDialog(nullptr), m_owner(parent), m_source(image->clone()), m_selection(selection) {
    setObjectName("strokeOpacityEditor"); setWindowTitle(tr("Edit Stroke Opacity Envelope"));
    auto *layout = new QGridLayout(this);
    m_model = new Model(this);
    const auto &first = m_source->getStroke(*selection.begin())->getOpacityProfile();
    for (int index : selection)
      if (m_source->getStroke(index)->getOpacityProfile() != first) m_mixed = true;
    m_model->setProfile(first);
    m_curve = new Curve(m_model, this); layout->addWidget(m_curve,0,0);
    m_preview = new VectorFillReview::Swatch(this); layout->addWidget(m_preview,0,1);
    m_table = new QTableView(this); m_table->setObjectName("strokeOpacityKnots");
    m_table->setModel(m_model); m_table->setItemDelegate(new NumberDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    layout->addWidget(m_table,1,0,1,2);
    m_curve->selectRow = [this](int row) { m_table->selectRow(row); };
    auto *actions = new QHBoxLayout;
    auto button = [&](const QString &text, const char *name, const std::function<void()> &action) {
      auto *b = new QPushButton(text,this); b->setObjectName(name); b->setAutoDefault(false); actions->addWidget(b);
      connect(b,&QPushButton::clicked,this,action);
    };
    button(tr("Add Knot"),"strokeOpacityAdd",[this]() { insert(false); });
    button(tr("Add Step"),"strokeOpacityStep",[this]() { insert(true); });
    button(tr("Remove"),"strokeOpacityRemove",[this]() {
      auto profile = m_model->profile();
      std::set<int,std::greater<int>> rows;
      for (const auto &index : m_table->selectionModel()->selectedRows()) rows.insert(index.row());
      for (int row : rows) profile.erase(profile.begin()+row);
      m_model->setProfile(profile);
    });
    button(tr("Clear Envelope"),"strokeOpacityClear",[this]() { m_model->setProfile({}); });
    button(tr("Fade In"),"strokeOpacityFadeIn",[this]() { m_model->setProfile({{0.,0.},{1.,1.}}); });
    button(tr("Fade Out"),"strokeOpacityFadeOut",[this]() { m_model->setProfile({{0.,1.},{1.,0.}}); });
    layout->addLayout(actions,2,0,1,2);
    m_status = new QLabel(this); m_status->setWordWrap(true); layout->addWidget(m_status,3,0,1,2);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel,this);
    m_apply = buttons->button(QDialogButtonBox::Apply); m_apply->setObjectName("strokeOpacityApply");
    m_apply->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setObjectName("strokeOpacityCancel");
    connect(m_apply,&QPushButton::clicked,this,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    layout->addWidget(buttons,4,0,1,2);
    m_previewTimer = new QTimer(this); m_previewTimer->setSingleShot(true); m_previewTimer->setInterval(80);
    connect(m_previewTimer,&QTimer::timeout,this,[this]() { preview(); });
    connect(this,&QDialog::finished,m_previewTimer,&QTimer::stop);
    auto changed = [this]() { m_changed = true; updateState(); };
    connect(m_model,&QAbstractItemModel::dataChanged,this,changed);
    connect(m_model,&QAbstractItemModel::modelReset,this,changed);
    auto *app = TTool::getApplication();
    if (app) {
      connect(app->getCurrentScene(),&TSceneHandle::sceneSwitched,this,&QDialog::reject);
      connect(app->getCurrentLevel(),&TXshLevelHandle::xshLevelSwitched,this,&QDialog::reject);
      connect(app->getCurrentFrame(),&TFrameHandle::frameSwitched,this,&QDialog::reject);
      connect(app->getCurrentColumn(),&TColumnHandle::columnIndexSwitched,this,&QDialog::reject);
      connect(app->getCurrentTool(),&ToolHandle::toolSwitched,this,&QDialog::reject);
    }
    updateState();
    m_preview->setImage(m_source,m_source->getBBox());
    resize(720,600);
    m_owner.watch(*this);
  }
  bool reviewValid() const { return m_owner.accepted(*this); }
  const StrokeOpacity::Profile &profile() const { return m_model->profile(); }
};
}  // namespace StrokeOpacityEditor
