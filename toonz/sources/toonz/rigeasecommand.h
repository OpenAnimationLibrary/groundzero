#pragma once

#ifndef RIGEASECOMMAND_H
#define RIGEASECOMMAND_H

#include "menubarcommandids.h"

#include <QtGlobal>

// Whole-rig breakdown and ease commands (Xsheet, Timeline, Cells menu).
// The tables below are the single list of presets: the commands are created
// from them in rigeasecommand.cpp, registered as actions in mainwindow.cpp
// and put in menus from them. They mirror the presets of the Function
// Spreadsheet (toonzqt/functionsheet.cpp).

// The new key's value as a percentage of the way from the previous key's
// value to the next one's.
struct RigBreakdownPreset {
  const char *id;
  const char *name;
  double favour;
};

inline constexpr RigBreakdownPreset cRigBreakdownPresets[] = {
    {MI_RigBreakdown25,
     QT_TRANSLATE_NOOP("MainWindow", "Favor Previous Key 25%"), 25},
    {MI_RigBreakdown33,
     QT_TRANSLATE_NOOP("MainWindow", "Favor Previous Key 33%"), 100.0 / 3},
    {MI_RigBreakdown50, QT_TRANSLATE_NOOP("MainWindow", "Halfway 50%"), 50},
    {MI_RigBreakdown66, QT_TRANSLATE_NOOP("MainWindow", "Favor Next Key 66%"),
     200.0 / 3},
    {MI_RigBreakdown75, QT_TRANSLATE_NOOP("MainWindow", "Favor Next Key 75%"),
     75},
    {MI_RigBreakdownAnticipate,
     QT_TRANSLATE_NOOP("MainWindow", "Anticipation -10%"), -10},
    {MI_RigBreakdownOvershoot,
     QT_TRANSLATE_NOOP("MainWindow", "Overshoot 110%"), 110}};

inline constexpr int cRigBreakdownPresetCount =
    int(sizeof(cRigBreakdownPresets) / sizeof(cRigBreakdownPresets[0]));

// Ease In / Ease Out (%) of a segment: easeOut leaves the segment's first key
// and easeIn reaches its last key, as percentages of the segment length.
struct RigEasePreset {
  const char *id;
  const char *name;
  double easeOut, easeIn;
};

inline constexpr RigEasePreset cRigEasePresets[] = {
    {MI_RigEaseInOut33,
     QT_TRANSLATE_NOOP("MainWindow", "Ease In / Ease Out 33%"), 100.0 / 3,
     100.0 / 3},
    {MI_RigEaseInOut50,
     QT_TRANSLATE_NOOP("MainWindow", "Ease In / Ease Out 50%"), 50, 50},
    {MI_RigEaseOut50, QT_TRANSLATE_NOOP("MainWindow", "Ease Out 50%"), 50, 0},
    {MI_RigEaseIn50, QT_TRANSLATE_NOOP("MainWindow", "Ease In 50%"), 0, 50},
    {MI_RigEaseOut100, QT_TRANSLATE_NOOP("MainWindow", "Ease Out 100%"), 100,
     0},
    {MI_RigEaseIn100, QT_TRANSLATE_NOOP("MainWindow", "Ease In 100%"), 0, 100}};

inline constexpr int cRigEasePresetCount =
    int(sizeof(cRigEasePresets) / sizeof(cRigEasePresets[0]));

#endif
