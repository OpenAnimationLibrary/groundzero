#pragma once

#ifndef DISTRIBUTECOMMAND_H
#define DISTRIBUTECOMMAND_H

class QObject;

//! Distribute to Layers, Distribute to Frames and Convert Selection to Symbol
//! (distributecommand.cpp) work on a selection of vector strokes in the viewer.
//! This keeps the three commands enabled only while such a selection exists;
//! \e context owns the connections to the application handles.
void initDistributeCommands(QObject *context);

#endif  // DISTRIBUTECOMMAND_H
