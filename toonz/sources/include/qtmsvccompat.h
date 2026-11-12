#pragma once
#include <QtCore/qglobal.h>

// Qt 5's older headers refer to Microsoft's removed stdext array iterators.
// Keep the standard pointer iterator used by Qt on other toolchains. This
// changes header-only algorithms, not the ABI of the WinTab-enabled Qt DLLs.
// Include before Qt container headers. Newer Qt versions need no override.
// See QTBUG-118993 and microsoft/STL#951.
#if defined(_MSC_VER) && _MSC_VER >= 1938 &&                                   \
    QT_VERSION < QT_VERSION_CHECK(5, 15, 17)
#undef QT_MAKE_CHECKED_ARRAY_ITERATOR
#undef QT_MAKE_UNCHECKED_ARRAY_ITERATOR
#define QT_MAKE_CHECKED_ARRAY_ITERATOR(pointer, count) (pointer)
#define QT_MAKE_UNCHECKED_ARRAY_ITERATOR(pointer) (pointer)
#endif
