#pragma once

#ifndef TCOLUMNFX_H
#define TCOLUMNFX_H

// TnzCore includes
#include "tthreadmessage.h"
#include "trastercm.h"

// TnzBase includes
#include "trasterfx.h"
#include "tbasefx.h"
#include "tdoubleparam.h"
#include "tnotanimatableparam.h"
#include "tparamset.h"

// TnzLib includes
#include "toonz/columncoloreffect.h"

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//=========================================================================

//    Forward declarations

class TXshLevelColumn;
class TXshPaletteColumn;
class TXshZeraryFxColumn;
class TXsheet;
class TXshColumn;
class TXshSimpleLevel;
class FxDag;
class TImageInfo;
class TOfflineGL;
class TVectorImageP;
class TVectorRenderData;

//=========================================================================

//*******************************************************************************************
//    TColumnFx  declaration
//*******************************************************************************************

class DVAPI TColumnFx : public TRasterFx {
public:
  TColumnFx() : TRasterFx() {}

  virtual int getColumnIndex() const         = 0;
  virtual std::wstring getColumnName() const = 0;
  virtual std::wstring getColumnId() const   = 0;
  virtual TXshColumn *getXshColumn() const   = 0;

  int getReferenceColumnIndex() const override { return getColumnIndex(); }
};

//*******************************************************************************************
//    TLevelColumnFx  declaration
//*******************************************************************************************

class DVAPI TLevelColumnFx : public TColumnFx {
  TXshLevelColumn *m_levelColumn;
  bool m_isCachable;

  TThread::Mutex m_mutex;
  TOfflineGL *m_offlineContext;

  // Time remap (master plan B7, see toonz/timeremap.h). The parameters are
  // saved only when the remap is used, so the scenes that do not use it are
  // saved as before.
  TDoubleParamP m_timeRemap;
  TBoolParamP m_timeRemapEnabled;

  // Color Effect (doc/animate_color_effect.md, see toonz/columncoloreffect.h):
  // Brightness, Tint, Alpha and Advanced. Each parameter is saved only when it
  // is not at its default, so the scenes that do not use it are saved as
  // before.
  TDoubleParamP m_colorEffect[ColumnColorEffect::ParamCount];
  TPixelParamP m_tintColor;

public:
  TLevelColumnFx();
  ~TLevelColumnFx();

  TFx *clone(bool recursive = true) const override;

  TPalette *getPalette(int frame) const;
  TFilePath getPalettePath(int frame) const;

  void setColumn(TXshLevelColumn *column);
  TXshLevelColumn *getColumn() const { return m_levelColumn; }

  //! The curve that tells, for each row of the output, the (1-based) xsheet
  //! frame whose cell is shown there. It is ignored when the remap is
  //! disabled or the curve has no keys.
  TDoubleParam *getTimeRemapParam() const { return m_timeRemap.getPointer(); }
  TBoolParam *getTimeRemapEnabledParam() const {
    return m_timeRemapEnabled.getPointer();
  }
  //! True if the column is shown with a retimed cell lookup.
  bool isTimeRemapActive() const;
  //! True if the remap has been used: it is enabled, or its curve has keys or
  //! is cycled. Unused remap parameters are saved only for editor metadata.
  bool isTimeRemapUsed() const;
  //! The row of the xsheet whose cell is shown at the row \b row of the output;
  //! \b row itself if the remap is not active.
  int getRemappedRow(int row) const;

  //! The parameter \b p (a ColumnColorEffect::Param) of the Color Effect.
  TDoubleParam *getColorEffectParam(int p) const;
  //! The tint color of the Color Effect (its alpha is not used).
  TPixelParam *getTintColorParam() const { return m_tintColor.getPointer(); }
  //! True if a parameter of the Color Effect is not at its default (it has
  //! keys, or another value). Unused parameters are saved only for editor metadata.
  bool isColorEffectUsed() const;
  //! The values of the Color Effect parameters at \b frame.
  ColumnColorEffect::Values getColorEffectValues(double frame) const;
  //! The Color Effect at \b frame, as a multiply and an offset per channel.
  ColumnColorEffect::Transform getColorEffect(double frame) const;

  std::wstring getColumnName() const override;
  std::wstring getColumnId() const override;
  int getColumnIndex() const override;
  TXshColumn *getXshColumn() const override;

  bool isCachable() const override { return m_isCachable; }

  bool canHandle(const TRenderSettings &info, double frame) override;
  TAffine handledAffine(const TRenderSettings &info, double frame) override;
  TAffine getDpiAff(int frame);

  TFxTimeRegion getTimeRegion() const override;
  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override;
  std::string getAlias(double frame,
                       const TRenderSettings &info) const override;
  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &info) override;

  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &info) override;
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &info) override;

  void saveData(TOStream &os) override;
  void loadData(TIStream &is) override;

  const TPersistDeclaration *getDeclaration() const override;
  std::string getPluginId() const override;

  bool isParamSaved(const TParamVar *paramVar) const override;

private:
  void getImageInfo(TImageInfo &imageInfo, TXshSimpleLevel *sl,
                    TFrameId frameId);

  TImageP applyTzpFxs(TToonzImageP &ti, double frame,
                      const TRenderSettings &info);
  void applyTzpFxsOnVector(const TVectorImageP &vi, TTile &tile, double frame,
                           const TRenderSettings &info);

private:
  // not implemented
  TLevelColumnFx(const TLevelColumnFx &);
  TLevelColumnFx &operator=(const TLevelColumnFx &);
};

//*******************************************************************************************
//    TPaletteColumnFx  declaration
//*******************************************************************************************

class TPaletteColumnFx : public TColumnFx {
  TXshPaletteColumn *m_paletteColumn;

public:
  TPaletteColumnFx();
  ~TPaletteColumnFx();

  TFx *clone(bool recursive = true) const override;

  TPalette *getPalette(int frame) const;
  TFilePath getPalettePath(int frame) const;

  void setColumn(TXshPaletteColumn *column) { m_paletteColumn = column; }
  TXshPaletteColumn *getColumn() const { return m_paletteColumn; }

  std::wstring getColumnName() const override;
  std::wstring getColumnId() const override;
  int getColumnIndex() const override;
  TXshColumn *getXshColumn() const override;

  bool isCachable() const override { return false; }

  bool canHandle(const TRenderSettings &info, double frame) override;

  TFxTimeRegion getTimeRegion() const override;
  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override;
  std::string getAlias(double frame,
                       const TRenderSettings &info) const override;
  TAffine getDpiAff(int frame);

  void doCompute(TTile &tile, double frame, const TRenderSettings &) override;

  const TPersistDeclaration *getDeclaration() const override;
  std::string getPluginId() const override;

private:
  // not implemented
  TPaletteColumnFx(const TPaletteColumnFx &);
  TPaletteColumnFx &operator=(const TPaletteColumnFx &);
};

//*******************************************************************************************
//    TZeraryColumnFx  declaration
//*******************************************************************************************

class DVAPI TZeraryColumnFx final : public TColumnFx {
  TXshZeraryFxColumn *m_zeraryFxColumn;
  TZeraryFx *m_fx;

public:
  TZeraryColumnFx();
  ~TZeraryColumnFx();

  TZeraryFx *getZeraryFx() const { return m_fx; }
  void setZeraryFx(TFx *fx);

  void setColumn(TXshZeraryFxColumn *column);
  TXshZeraryFxColumn *getColumn() const { return m_zeraryFxColumn; }

  std::wstring getColumnName() const override;
  std::wstring getColumnId() const override;
  int getColumnIndex() const override;
  TXshColumn *getXshColumn() const override;

  bool canHandle(const TRenderSettings &info, double frame) override {
    return true;
  }

  TFxTimeRegion getTimeRegion() const override;
  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override;
  std::string getAlias(double frame,
                       const TRenderSettings &info) const override;

  void doCompute(TTile &tile, double frame, const TRenderSettings &) override;

  void saveData(TOStream &os) override;
  void loadData(TIStream &is) override;

  const TPersistDeclaration *getDeclaration() const override;
  std::string getPluginId() const override;

private:
  // not implemented
  TZeraryColumnFx(const TZeraryColumnFx &);
  TZeraryColumnFx &operator=(const TZeraryColumnFx &);
};

//*******************************************************************************************
//    TXsheetFx  declaration
//*******************************************************************************************

class TXsheetFx final : public TRasterFx {
  FxDag *m_fxDag;

public:
  TXsheetFx();

  FxDag *getFxDag() const { return m_fxDag; }

  bool canHandle(const TRenderSettings &info, double frame) override {
    return false;
  }

  std::string getAlias(double frame,
                       const TRenderSettings &info) const override;

  void doCompute(TTile &tile, double frame, const TRenderSettings &) override;
  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override;

  const TPersistDeclaration *getDeclaration() const override;
  std::string getPluginId() const override;

private:
  friend class FxDag;
  void setFxDag(FxDag *fxDag);

  // not implemented
  TXsheetFx(const TXsheetFx &);
  TXsheetFx &operator=(const TXsheetFx &);
};

//*******************************************************************************************
//    TOutputFx  declaration
//*******************************************************************************************

class TOutputFx final : public TRasterFx {
  TRasterFxPort m_input;

public:
  TOutputFx();

  bool canHandle(const TRenderSettings &info, double frame) override {
    return false;
  }

  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override;

  void doCompute(TTile &tile, double frame, const TRenderSettings &) override;

  const TPersistDeclaration *getDeclaration() const override;
  std::string getPluginId() const override;

private:
  // not implemented
  TOutputFx(const TOutputFx &);
  TOutputFx &operator=(const TOutputFx &);
};

//*******************************************************************************************
//    TColumnFx functions
//*******************************************************************************************

bool isSubsheetChainOnColumn0(TXsheet *topXsheet, TXsheet *subsheet, int frame);

#endif  // TCOLUMNFX_H
