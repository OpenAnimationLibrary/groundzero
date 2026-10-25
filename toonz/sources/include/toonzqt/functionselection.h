#pragma once

#ifndef FUNCTIONSELECTION_H
#define FUNCTIONSELECTION_H

#include "tcommon.h"
#include "functiontreeviewer.h"
#include "toonzqt/selection.h"
#include "toonzqt/dvmimedata.h"

#include "tdoublekeyframe.h"

#include <QWidget>
#include <QList>
#include <QSet>

#undef DVAPI
#undef DVVAR
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

// forward declaration
class TDoubleParam;
class TFrameHandle;

//-----------------------------------------------------------------------------

class ColumnToCurveMapper {
public:
  virtual TDoubleParam *getCurve(int columnIndex) const = 0;
};

//-----------------------------------------------------------------------------

class FunctionSelection final : public QObject, public TSelection {
  Q_OBJECT
  QRect m_selectedCells;  // yrange = rowrange of the selected keyframes;
                          // xrange = columnrange (functionsheet only)
  QList<QPair<TDoubleParam *, QSet<int>>> m_selectedKeyframes;
  // first = curve, second = set of selected keyframes index

  int m_selectedSegment;  // index of the first keyframe of the segment; -1 if
                          // no segment selected
                          // (functionpanel only)
  // assert(m_selectedSegment<0 || m_selectedKeyframes.size()==1)

  TFrameHandle *m_frameHandle;
  ColumnToCurveMapper *m_columnToCurveMapper;

  int getCurveIndex(TDoubleParam *curve) const;
  // finds i : m_selectedKeyframes[i].first == curve
  //-1 if curve not found
  int touchCurveIndex(TDoubleParam *curve);
  // as getCurve(); if curve not found then add it

  // returns the first keyframe index of every selected segment, per curve.
  // if inclusive == true, also the segment overlapping the top selected row
  QList<QPair<TDoubleParam *, QList<int>>> getSelectedSegments(
      bool inclusive) const;

public:
  FunctionSelection();
  ~FunctionSelection();

  void setFrameHandle(TFrameHandle *frameHandle) {
    m_frameHandle = frameHandle;
  }

  // function graph
  void selectCurve(TDoubleParam *curve);
  void deselectAllKeyframes();

  // function sheet
  QRect getSelectedCells() const { return m_selectedCells; }
  void selectCells(const QRect &selectedCells,
                   const QList<TDoubleParam *> &curves);
  void selectCells(const QRect &selectedCells);
  void deselectAllCells();

  bool isEmpty() const override { return m_selectedKeyframes.empty(); }
  void selectNone() override;
  void select(TDoubleParam *curve, int k);
  bool isSelected(TDoubleParam *curve, int k) const;
  void selectSegment(TDoubleParam *, int k,
                     QRect selectedCells = QRect());  // note: if a segment is
                                                      // selected then also the
                                                      // segment ends are
                                                      // selected
  int getSelectedKeyframeCount() const;
  QPair<TDoubleParam *, int> getSelectedKeyframe(int index)
      const;  // if index<0 || index>=getSelectedKeyframeCount() returns (0,-1)

  QPair<TDoubleParam *, int> getSelectedSegment()
      const;  // if no segment is selected returns (0,-1)
  bool isSegmentSelected(TDoubleParam *, int k) const;

  void setColumnToCurveMapper(ColumnToCurveMapper *mapper);  // gets ownership

  TDoubleParam *getCurveFromColumn(int columnIndex) const {
    return m_columnToCurveMapper ? m_columnToCurveMapper->getCurve(columnIndex)
                                 : 0;
  }

  void enableCommands() override;

  void doCopy();
  void doPaste();
  void doCut();
  void doDelete();
  void insertCells();

  // if inclusive == true, consider all segments overlapping the selection
  void setStep(int, bool inclusive = true);
  void setStep1() { setStep(1); }
  void setStep2() { setStep(2); }
  void setStep3() { setStep(3); }
  void setStep4() { setStep(4); }

  // return step if all the selected segments has the same value.
  // return -1 if the selection does not overlap any segments
  // return 0 if the step value is uneven in the selection
  // if inclusive == true, consider all segments overlapping the selection
  int getCommonStep(bool inclusive = true);

  void setSegmentType(TDoubleKeyframe::Type type, bool inclusive = true);

  // set every selected segment to Ease In / Ease Out (%) interpolation with
  // the given percentages of the segment length. easeOut leaves the first key
  // of the segment and easeIn reaches the next one; their sum is cropped to
  // 100.
  void setSegmentEase(double easeOut, double easeIn, bool inclusive = true);

  // on every selected curve with keys before and after \b frame, set a key at
  // \b frame whose value is \b favour of the way from the previous key's
  // value to the next one's (0 = previous, 1 = next). Values outside [0, 1]
  // give an anticipation or an overshoot. Curves with a key at frame, or
  // whose segment there is an Expression, Similar Shape or File one, are left
  // unchanged.
  void insertBreakdowns(int frame, double favour);

  // return TDoubleKeyframe::Type value if the selected segments has the same
  // interpolation type. return -1 if the selection does not overlap any
  // segments return 0 (TDoubleKeyframe::None) if the interpolation is not
  // identical in the selection if inclusive == true, consider all segments
  // overlapping the selection
  int getCommonSegmentType(bool inclusive = true);

  QList<int> getSelectedKeyIndices(TDoubleParam *curve);
signals:
  void selectionChanged();
};

//-----------------------------------------------------------------------------

class FunctionKeyframesData final : public DvMimeData {
public:
  FunctionKeyframesData();
  ~FunctionKeyframesData();

  typedef std::vector<TDoubleKeyframe> Keyframes;

  void setColumnCount(int columnCount);
  int getColumnCount() const { return (int)m_keyframes.size(); }

  int getRowCount() const;

  void getData(int columnIndex, TDoubleParam *curve, double frame,
               const QSet<int> &kIndices);
  void setData(int columnIndex, TDoubleParam *curve, double frame) const;

  const Keyframes &getKeyframes(int columnIndex) const;

  DvMimeData *clone() const override;

  bool isCircularReferenceFree(int columnIndex, TDoubleParam *curve) const;

private:
  std::vector<Keyframes> m_keyframes;
};

#endif
