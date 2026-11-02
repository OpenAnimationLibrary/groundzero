#pragma once

#include <QDialog>
#include <QWidget>

class PluginManager;
class QCheckBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QShowEvent;

class PluginManagerPopup final : public QDialog {
  Q_OBJECT
public:
  explicit PluginManagerPopup(PluginManager *manager,
                              QWidget *parent = nullptr);

protected:
  void showEvent(QShowEvent *event) override;

private:
  void refresh();
  void install(bool folder);
  PluginManager *m_manager;
  QCheckBox *m_bypass;
  QLabel *m_status;
  QTreeWidget *m_plugins;
};

class PluginSettingsWidget final : public QWidget {
  Q_OBJECT
public:
  explicit PluginSettingsWidget(PluginManager *manager,
                                QWidget *parent = nullptr);

protected:
  void showEvent(QShowEvent *event) override;

private:
  void refresh();
  PluginManager *m_manager;
  QCheckBox *m_bypass;
  QLabel *m_status;
};
