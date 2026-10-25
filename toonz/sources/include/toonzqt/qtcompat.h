#pragma once

#ifndef QTCOMPAT_H
#define QTCOMPAT_H

//  Spellings that differ between Qt 5 and Qt 6, for the code built with both.

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#else
#include <QEvent>
#endif

//! The argument of QWidget::enterEvent(): a QEnterEvent in Qt 6, a QEvent in
//! Qt 5.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef QEnterEvent TEnterEvent;
#else
typedef QEvent TEnterEvent;
#endif

#endif
