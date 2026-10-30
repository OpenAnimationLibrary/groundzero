#pragma once

#if defined(__cplusplus) && defined(_MSC_VER)
#include <QtCore/qglobal.h>

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0) && \
    QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// Qt 5.15.2 maps these macros to non-standard stdext iterator factories:
// https://github.com/qt/qtbase/blob/v5.15.2/src/corelib/global/qcompilerdetection.h
// Microsoft removed those factories in STL PR 5817 (2025-11-05):
// https://github.com/microsoft/STL/pull/5817
// Load Qt's compiler configuration first, then use its existing portable
// pointer fallback before container headers such as QList are parsed.
#undef QT_MAKE_UNCHECKED_ARRAY_ITERATOR
#define QT_MAKE_UNCHECKED_ARRAY_ITERATOR(x) (x)
#undef QT_MAKE_CHECKED_ARRAY_ITERATOR
#define QT_MAKE_CHECKED_ARRAY_ITERATOR(x, N) (x)
#endif
#endif
