#pragma once

#ifndef XSHEETEXPR_INCLUDED
#define XSHEETEXPR_INCLUDED

#include "tgrammar.h"
#include <QSet>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//=============================================================================
// forward declarations
class TXsheet;
class TDoubleParam;
class TExpression;

DVAPI TSyntax::Grammar *createXsheetGrammar(TXsheet *xsh);
DVAPI bool dependsOn(TExpression &expr, TDoubleParam *possiblyDependentParam);
DVAPI bool dependsOn(TDoubleParam *param, TDoubleParam *possiblyDependentParam);
DVAPI void referenceParams(TExpression &expr, QSet<int> &columnIndices,
                           QSet<TDoubleParam *> &params);
// Resolve only a literal native channel reference, never an expression which
// happens to use one parameter. Offset layers are reported separately because
// copying the base curve alone would not preserve the effective channel value.
DVAPI bool referenceCurveForSnapshot(TExpression &expr, TDoubleParam *&param,
                                    bool &hasOffsetLayers);

#endif  // XSHEETEXPR_INCLUDED
