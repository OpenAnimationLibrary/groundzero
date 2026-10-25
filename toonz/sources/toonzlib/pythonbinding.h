#pragma once

#ifndef PYTHONBINDING_H
#define PYTHONBINDING_H

// The opentoonz Python module (built only with the CMake option WITH_PYTHON).
// Private to toonzlib.
//
// To be included first: Python.h must come before the system headers, and
// before the Qt ones, whose "slots" keyword breaks Python's object.h.

#pragma push_macro("slots")
#undef slots
#include <pybind11/pybind11.h>
#pragma pop_macro("slots")

namespace PythonBinding {

// Adds the Scene, Level and StageObject classes to the opentoonz module
void bindClasses(pybind11::module_ &m);

}  // namespace PythonBinding

#endif  // PYTHONBINDING_H
