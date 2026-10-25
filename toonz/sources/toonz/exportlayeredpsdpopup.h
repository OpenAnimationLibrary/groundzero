#pragma once

#ifndef EXPORTLAYEREDPSDPOPUP_H
#define EXPORTLAYEREDPSDPOPUP_H

// Tnz6 includes
#include "toonzqt/dvdialog.h"

namespace DVGui {
class FileField;
class IntLineEdit;
}  // namespace DVGui

class QCheckBox;
class QLineEdit;

//*********************************************************************************
//    Export Layered PSD Popup  declaration
//*********************************************************************************

//! Renders a range of frames of the current xsheet, one layered PSD file for
//! each frame, with one layer for each column (see doc/layered_psd_export.md).
class ExportLayeredPsdPopup final : public DVGui::Dialog {
  Q_OBJECT

  DVGui::FileField *m_folderField = nullptr;
  QLineEdit *m_nameField          = nullptr;
  DVGui::IntLineEdit *m_fromField = nullptr;
  DVGui::IntLineEdit *m_toField   = nullptr;
  QCheckBox *m_hiddenCB           = nullptr;

public:
  ExportLayeredPsdPopup();

protected:
  void showEvent(QShowEvent *event) override;

private:
  void initialize();
  void saveSettings();

private slots:
  void onExport();
};

#endif  // EXPORTLAYEREDPSDPOPUP_H
