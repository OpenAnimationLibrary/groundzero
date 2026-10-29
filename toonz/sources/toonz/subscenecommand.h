#pragma once

#ifndef SUBSCENECOMMAND_H
#define SUBSCENECOMMAND_H

#include "toonz/tstageobjectid.h"
#include "tfx.h"
#include <set>
#include <string>
#include <QList>
#include <QString>

class TXsheet;

namespace SubsceneCmd {

//! Collapses the columns into a new sub-xsheet. If \b levelName is not empty
//! it is used (made unique) as the name of the new sub-xsheet level.
void collapse(std::set<int> &indices, const std::wstring &levelName = L"");
void collapse(const QList<TStageObjectId> &objects);
void collapse(const QList<TFxP> &fxs);
void explode(int index);
void saveAsTemplate(const std::set<int> &indices);
//! Saves the xsheet as the template \b name of the Library (see
//! LibraryCmd), as saveAsTemplate() saves the columns it copies: the files of
//! the levels used by the xsheet are copied into a folder next to the
//! template and referred to with $scenefolder, and the icon and the .libinfo
//! file of \b kind are written. Asks to save the scene first if some of
//! those levels are not saved. Shows the messages; returns true if saved.
bool saveXsheetAsTemplate(TXsheet *xsh, const QString &name, const char *kind);
}

#endif
