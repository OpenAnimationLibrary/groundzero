#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QRect>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTreeWidget;
class WidgetInspectorPickSession;

// Read-only widget metadata snapshots. The inspected widget is never owned,
// modified, serialized or copied into a diagnostic log.
class WidgetInspector final : public QWidget {
  Q_OBJECT

public:
  using PanelTypeResolver = std::function<QString(QWidget *)>;

  explicit WidgetInspector(QWidget *parent = nullptr);
  ~WidgetInspector() override;

  void setPanelTypeResolver(PanelTypeResolver resolver);
  QWidget *inspectedWidget() const;
  bool isPicking() const;

public slots:
  void beginPicking();
  void cancelPicking();
  void inspectWidget(QWidget *widget);
  void refreshInspection();
  void clearInspection();

protected:
  void changeEvent(QEvent *event) override;
  void hideEvent(QHideEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

private:
  friend class WidgetInspectorPickSession;
  void pickingFinished();
  void updateText();
  void showSnapshot();

  QPushButton *m_pickButton;
  QPushButton *m_refreshButton;
  QPushButton *m_clearButton;
  QLabel *m_status;
  QTreeWidget *m_properties;
  QPointer<QWidget> m_target;
  QPointer<WidgetInspectorPickSession> m_picker;
  QMetaObject::Connection m_destroyedConnection;
  PanelTypeResolver m_panelTypeResolver;

  QString m_className;
  QString m_objectName;
  QString m_panelType;
  QRect m_geometry;
  bool m_visible = false;
  bool m_enabled = false;
};
