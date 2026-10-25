#pragma once

#ifndef QTCOMPAT_H
#define QTCOMPAT_H

//  Spellings that differ between Qt 5 and Qt 6, for the code built with both.

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#include <QInputDevice>
#include <QPointingDevice>
#else
#include <QEvent>
#include <QTouchDevice>
#endif
#include <QAction>
#include <QMenu>
#include <QTabletEvent>

//! The argument of QWidget::enterEvent(): a QEnterEvent in Qt 6, a QEvent in
//! Qt 5.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef QEnterEvent TEnterEvent;
#else
typedef QEvent TEnterEvent;
#endif

//! The result argument of QAbstractNativeEventFilter::nativeEventFilter() and
//! QWidget::nativeEvent(): a long in Qt 5, a qintptr in Qt 6.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef qintptr TNativeEventResult;
#else
typedef long TNativeEventResult;
#endif

//! The type of the device of a QTouchEvent: QTouchDevice::DeviceType in Qt 5,
//! QInputDevice::DeviceType in Qt 6. Name the values through the type, as in
//! TTouchDeviceType::TouchScreen, which both versions accept.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef QInputDevice::DeviceType TTouchDeviceType;
#else
typedef QTouchDevice::DeviceType TTouchDeviceType;
#endif

//! The pointer type of a QTabletEvent: QTabletEvent::PointerType in Qt 5,
//! QPointingDevice::PointerType in Qt 6. Pen, Cursor and Eraser have the same
//! names in both, as in TTabletPointerType::Eraser.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef QPointingDevice::PointerType TTabletPointerType;
#else
typedef QTabletEvent::PointerType TTabletPointerType;
#endif

//! Whether the pointer of a tablet event is known (a pen, cursor or eraser).
inline bool isKnownTabletPointer(const QTabletEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return e->pointerType() != QPointingDevice::PointerType::Unknown;
#else
  return e->pointerType() != QTabletEvent::UnknownPointer;
#endif
}

//! The menu of an action, or 0: QAction::menu() returns a QMenu * in Qt 5 but
//! a QObject * by default in Qt 6 (QAction is in QtGui there).
inline QMenu *menuOfAction(const QAction *action) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return action->menu<QMenu *>();
#else
  return action->menu();
#endif
}

#endif
