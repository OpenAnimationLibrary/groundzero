#pragma once

#ifndef SUBSCENECOMMAND_H
#define SUBSCENECOMMAND_H

#include "toonz/tstageobjectid.h"
#include "tfx.h"
#include <set>
#include <string>
#include <QList>

namespace SubsceneCmd {

//! Collapses the columns into a new sub-xsheet. If \b levelName is not empty
//! it is used (made unique) as the name of the new sub-xsheet level.
void collapse(std::set<int> &indices, const std::wstring &levelName = L"");
void collapse(const QList<TStageObjectId> &objects);
void collapse(const QList<TFxP> &fxs);
void explode(int index);
}

#endif
