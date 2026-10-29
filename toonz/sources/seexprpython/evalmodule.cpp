#include "evalmodule.h"
#include "tseexpr.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct AcquireGIL {
  PyGILState_STATE state = PyGILState_Ensure();
  ~AcquireGIL() { PyGILState_Release(state); }
};

std::string stringValue(PyObject *object) {
  Py_ssize_t size = 0;
  const char *bytes = PyUnicode_AsUTF8AndSize(object, &size);
  if (!bytes) throw std::runtime_error("Expected a Unicode string");
  return std::string(bytes, static_cast<std::size_t>(size));
}

TSeExpr::Type resultType(const char *name) {
  const std::string type(name);
  if (type == "scalar") return TSeExpr::Type::Scalar;
  if (type == "vector") return TSeExpr::Type::Vector;
  if (type == "string") return TSeExpr::Type::String;
  throw std::runtime_error("Result type must be scalar, vector or string");
}

TSeExpr::Value nativeValue(PyObject *object, bool preserveVector = false) {
  if (PyUnicode_Check(object)) return TSeExpr::Value::string(stringValue(object));
  if (PyFloat_Check(object) || PyLong_Check(object)) {
    const double number = PyFloat_AsDouble(object);
    if (PyErr_Occurred()) throw std::runtime_error("Invalid scalar value");
    return TSeExpr::Value::scalar(number);
  }
  PyObject *sequence = PySequence_Fast(object, "A vector must be a sequence of numbers");
  if (!sequence) throw std::runtime_error("Expected a number, numeric vector or string");
  const Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
  if (count < 1 || count > 16) {
    Py_DECREF(sequence);
    throw std::runtime_error("A vector must contain one to sixteen numbers");
  }
  std::vector<double> numbers;
  numbers.reserve(static_cast<std::size_t>(count));
  for (Py_ssize_t i = 0; i < count; ++i) {
    const double value = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, i));
    if (PyErr_Occurred()) {
      Py_DECREF(sequence);
      throw std::runtime_error("Vector components must be numbers");
    }
    numbers.push_back(value);
  }
  Py_DECREF(sequence);
  auto result = TSeExpr::Value::numeric(std::move(numbers));
  // A typed Vector[1] callback is still a vector. Variable sequences retain
  // the adapter's ordinary dimension-one scalar inference.
  if (preserveVector) result.type = TSeExpr::Type::Vector;
  return result;
}

PyObject *pythonValue(const TSeExpr::Value &value) {
  if (value.type == TSeExpr::Type::String)
    return PyUnicode_DecodeUTF8(value.text.data(), value.text.size(), "strict");
  if (value.type == TSeExpr::Type::Scalar)
    return PyFloat_FromDouble(value.number[0]);
  PyObject *result = PyTuple_New(static_cast<Py_ssize_t>(value.number.size()));
  if (!result) return nullptr;
  for (std::size_t i = 0; i < value.number.size(); ++i) {
    PyObject *item = PyFloat_FromDouble(value.number[i]);
    if (!item) { Py_DECREF(result); return nullptr; }
    PyTuple_SET_ITEM(result, i, item);
  }
  return result;
}

std::vector<TSeExpr::Variable> variables(PyObject *dictionary) {
  std::vector<TSeExpr::Variable> result;
  if (!dictionary || dictionary == Py_None) return result;
  if (!PyDict_Check(dictionary)) throw std::runtime_error("Variables must be a dictionary");
  PyObject *key, *value;
  Py_ssize_t position = 0;
  while (PyDict_Next(dictionary, &position, &key, &value)) {
    if (!PyUnicode_Check(key)) throw std::runtime_error("Variable names must be strings");
    result.push_back({stringValue(key), nativeValue(value)});
  }
  return result;
}

std::string pythonError() {
  PyObject *type = nullptr, *value = nullptr, *trace = nullptr;
  PyErr_Fetch(&type, &value, &trace);
  PyErr_NormalizeException(&type, &value, &trace);
  PyObject *description = value ? PyObject_Str(value) : nullptr;
  std::string message = "Python host callback failed";
  if (description) {
    const char *text = PyUnicode_AsUTF8(description);
    if (text) message = text;
  }
  Py_XDECREF(description); Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(trace);
  PyErr_Clear();
  return message;
}

std::vector<TSeExpr::Function> functions(PyObject *dictionary) {
  std::vector<TSeExpr::Function> result;
  if (!dictionary || dictionary == Py_None) return result;
  if (!PyDict_Check(dictionary)) throw std::runtime_error("Functions must be a dictionary");
  PyObject *key, *definition;
  Py_ssize_t position = 0;
  while (PyDict_Next(dictionary, &position, &key, &definition)) {
    if (!PyUnicode_Check(key) || !PyDict_Check(definition))
      throw std::runtime_error("Functions require named signature dictionaries");
    PyObject *callback = PyDict_GetItemString(definition, "callback");
    if (!callback || !PyCallable_Check(callback))
      throw std::runtime_error("A host function requires a callable");
    TSeExpr::Function function;
    function.name = stringValue(key);
    PyObject *type = PyDict_GetItemString(definition, "result_type");
    function.resultType = type ? resultType(stringValue(type).c_str()) : TSeExpr::Type::Scalar;
    PyObject *dimension = PyDict_GetItemString(definition, "dimension");
    if (dimension) {
      const long value = PyLong_AsLong(dimension);
      if (PyErr_Occurred() || value < 1 || value > 16)
        throw std::runtime_error("Invalid function result dimension");
      function.resultDimension = static_cast<int>(value);
    }
    PyObject *signature = PyDict_GetItemString(definition, "arguments");
    if (!signature || !PyList_Check(signature))
      throw std::runtime_error("A host function requires an arguments list");
    for (Py_ssize_t i = 0; i < PyList_Size(signature); ++i) {
      PyObject *argument = PyList_GetItem(signature, i);
      if (!PyUnicode_Check(argument)) throw std::runtime_error("Argument types must be strings");
      function.argumentTypes.push_back(resultType(stringValue(argument).c_str()));
    }
    PyObject *dimensions = PyDict_GetItemString(definition, "argument_dimensions");
    if (dimensions) {
      if (!PyList_Check(dimensions) || PyList_Size(dimensions) != PyList_Size(signature))
        throw std::runtime_error("Argument dimensions must match the arguments list");
      for (Py_ssize_t i = 0; i < PyList_Size(dimensions); ++i) {
        const long value = PyLong_AsLong(PyList_GetItem(dimensions, i));
        if (PyErr_Occurred() || value < 1 || value > 16)
          throw std::runtime_error("Argument dimensions must be one to sixteen");
        function.argumentDimensions.push_back(static_cast<int>(value));
      }
    }
    PyObject *minimum = PyDict_GetItemString(definition, "min_args");
    PyObject *maximum = PyDict_GetItemString(definition, "max_args");
    if (minimum) {
      const long value = PyLong_AsLong(minimum);
      if (PyErr_Occurred() || value < -1 || value > 1024)
        throw std::runtime_error("Invalid minimum host function argument count");
      function.minArgs = static_cast<int>(value);
    }
    if (maximum) {
      const long value = PyLong_AsLong(maximum);
      if (PyErr_Occurred() || value < -1 || value > 1024)
        throw std::runtime_error("Invalid maximum host function argument count");
      function.maxArgs = static_cast<int>(value);
    }
    if (PyErr_Occurred()) throw std::runtime_error("Invalid host function argument count");
    PyObject *documentation = PyDict_GetItemString(definition, "documentation");
    if (documentation) function.documentation = stringValue(documentation);
    Py_INCREF(callback);
    auto retained = std::shared_ptr<PyObject>(callback, [](PyObject *object) {
      AcquireGIL guard;
      Py_DECREF(object);
    });
    const bool vectorResult = function.resultType == TSeExpr::Type::Vector;
    function.evaluate = [retained, vectorResult](const std::vector<TSeExpr::Value> &values) {
      AcquireGIL guard;
      PyObject *arguments = PyTuple_New(static_cast<Py_ssize_t>(values.size()));
      if (!arguments) {
        const auto message = pythonError();
        throw std::runtime_error(message);
      }
      for (std::size_t i = 0; i < values.size(); ++i) {
        PyObject *value = pythonValue(values[i]);
        if (!value) {
          Py_DECREF(arguments);
          const auto message = pythonError();
          throw std::runtime_error(message);
        }
        PyTuple_SET_ITEM(arguments, i, value);
      }
      PyObject *answer = PyObject_CallObject(retained.get(), arguments);
      Py_DECREF(arguments);
      if (!answer) {
        const auto message = pythonError();
        throw std::runtime_error(message);
      }
      try {
        auto value = nativeValue(answer, vectorResult);
        Py_DECREF(answer);
        return value;
      } catch (...) {
        Py_DECREF(answer);
        if (PyErr_Occurred()) PyErr_Clear();
        throw;
      }
    };
    result.push_back(std::move(function));
  }
  return result;
}

PyObject *evaluate(PyObject *, PyObject *args, PyObject *keywords, bool inspect) {
  PyObject *source = nullptr, *values = Py_None, *callbacks = Py_None;
  const char *type = "vector", *backend = "interpreter";
  int dimension = 3;
  static const char *names[] = {"source", "variables", "result_type", "dimension",
                               "backend", "functions", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args, keywords, "U|OsisO", const_cast<char **>(names),
                                  &source, &values, &type, &dimension, &backend, &callbacks))
    return nullptr;
  try {
    TSeExpr::Options options;
    options.resultType = resultType(type);
    options.resultDimension = dimension;
    const std::string selected(backend);
    if (selected == "llvm") options.backend = TSeExpr::Backend::LLVM;
    else if (selected != "interpreter") throw std::runtime_error("Unknown evaluation backend");
    TSeExpr::Program program;
    const bool valid = program.compile(stringValue(source), variables(values),
                                       functions(callbacks), options);
    if (PyErr_Occurred()) PyErr_Clear();
    if (inspect) {
      PyObject *errors = PyList_New(0);
      if (!errors) return nullptr;
      for (const auto &error : program.errors()) {
        PyObject *item = Py_BuildValue("{s:s,s:i,s:i}", "message", error.message.c_str(),
                                      "begin", error.begin, "end", error.end);
        if (!item || PyList_Append(errors, item) < 0) {
          Py_XDECREF(item); Py_DECREF(errors); return nullptr;
        }
        Py_DECREF(item);
      }
      PyObject *result = Py_BuildValue("{s:O,s:O}", "valid", valid ? Py_True : Py_False,
                                      "errors", errors);
      Py_DECREF(errors);
      return result;
    }
    if (!valid) throw std::runtime_error(program.errorMessage());
    TSeExpr::Value value;
    bool evaluated;
    {
      struct ReleaseGIL {
        PyThreadState *state = PyEval_SaveThread();
        ~ReleaseGIL() { PyEval_RestoreThread(state); }
      } release;
      evaluated = program.evaluate(value);
    }
    if (!evaluated) throw std::runtime_error(program.errorMessage());
    return pythonValue(value);
  } catch (const std::exception &error) {
    PyErr_Clear();
    PyErr_SetString(PyExc_RuntimeError, error.what());
    return nullptr;
  } catch (...) {
    PyErr_Clear();
    PyErr_SetString(PyExc_RuntimeError, "Expression evaluation failed");
    return nullptr;
  }
}

PyObject *evaluateBinding(PyObject *self, PyObject *args, PyObject *keywords) {
  return evaluate(self, args, keywords, false);
}
PyObject *validateBinding(PyObject *self, PyObject *args, PyObject *keywords) {
  return evaluate(self, args, keywords, true);
}
PyObject *catalog(bool native) {
  PyObject *result = PyList_New(0);
  if (!result) return nullptr;
  try {
    const auto entries = native ? TSeExpr::Program::nativeFunctions()
                                : TSeExpr::Program::builtinFunctions();
    for (const auto &function : entries) {
      PyObject *entry = Py_BuildValue("{s:s,s:s,s:i,s:i}", "name", function.name.c_str(),
          "documentation", function.documentation.c_str(), "min_args", function.minArgs,
          "max_args", function.maxArgs);
      if (!entry || PyList_Append(result, entry) < 0) {
        Py_XDECREF(entry); Py_DECREF(result); return nullptr;
      }
      Py_DECREF(entry);
    }
    return result;
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "Expression function catalog is unavailable");
  }
  Py_DECREF(result);
  return nullptr;
}
PyObject *catalogBinding(PyObject *, PyObject *) { return catalog(false); }
PyObject *nativeCatalogBinding(PyObject *, PyObject *) { return catalog(true); }
PyMethodDef methods[] = {
    {"evaluate", reinterpret_cast<PyCFunction>(evaluateBinding), METH_VARARGS | METH_KEYWORDS,
     "Evaluate a formula using the native expression engine."},
    {"validate", reinterpret_cast<PyCFunction>(validateBinding), METH_VARARGS | METH_KEYWORDS,
     "Validate a formula and return native diagnostics."},
    {"functions", catalogBinding, METH_NOARGS, "Return the actual registered function catalog."},
    {"native_functions", nativeCatalogBinding, METH_NOARGS,
     "Return native extension functions separately from the core catalog."},
    {nullptr, nullptr, 0, nullptr}};
}  // namespace

int addEvaluationBindings(PyObject *module) { return PyModule_AddFunctions(module, methods); }
