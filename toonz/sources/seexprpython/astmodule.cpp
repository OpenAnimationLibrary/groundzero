#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <SeExpr2/parser/SeExprParse.h>
#include <SeExpr2/parser/ASTNode.h>

#include <exception>
#include <mutex>
#include <string>

namespace {
// The upstream inspection parser initializes a process-global token table.
// Keep its initialization and parsing serialized independently of evaluation.
std::mutex parserMutex;

PyObject *text(const std::string &value) {
  return PyUnicode_DecodeUTF8(value.data(), static_cast<Py_ssize_t>(value.size()),
                             "strict");
}

PyObject *nodeValue(const ASTNode &node) {
  switch (node.type()) {
  case ASTType::Assign:
    return text(static_cast<const ASTPolicy::Assign &>(node).value());
  case ASTType::Var:
    return text(static_cast<const ASTPolicy::Var &>(node).value());
  case ASTType::Call:
    return text(static_cast<const ASTPolicy::Call &>(node).value());
  case ASTType::String:
    return text(static_cast<const ASTPolicy::String &>(node).value());
  case ASTType::Num:
    return PyFloat_FromDouble(static_cast<const ASTPolicy::Num &>(node).value());
  case ASTType::BinaryOp: {
    const char op = static_cast<const ASTPolicy::BinaryOp &>(node).value();
    return PyUnicode_FromStringAndSize(&op, 1);
  }
  case ASTType::UnaryOp: {
    const char op = static_cast<const ASTPolicy::UnaryOp &>(node).value();
    return PyUnicode_FromStringAndSize(&op, 1);
  }
  default:
    Py_RETURN_NONE;
  }
}

PyObject *describe(const ASTNode &node, unsigned depth = 0) {
  if (depth > 128) {
    PyErr_SetString(PyExc_RuntimeError, "Expression inspection depth exceeded");
    return nullptr;
  }
  PyObject *value = nodeValue(node);
  if (!value) return nullptr;
  PyObject *children = PyList_New(0);
  if (!children) { Py_DECREF(value); return nullptr; }
  for (const auto &child : node.children()) {
    PyObject *item = describe(*child, depth + 1);
    if (!item || PyList_Append(children, item) < 0) {
      Py_XDECREF(item); Py_DECREF(value); Py_DECREF(children);
      return nullptr;
    }
    Py_DECREF(item);
  }
  const auto &range = node.range();
  PyObject *result = Py_BuildValue("{s:i,s:O,s:(ii),s:O}",
      "type", static_cast<int>(node.type()), "value", value,
      "range", range[0], range[1], "children", children);
  Py_DECREF(value);
  Py_DECREF(children);
  return result;
}

PyObject *parse(PyObject *, PyObject *argument) {
  if (!PyUnicode_Check(argument)) {
    PyErr_SetString(PyExc_TypeError, "Expression source must be a string");
    return nullptr;
  }
  Py_ssize_t size = 0;
  const char *source = PyUnicode_AsUTF8AndSize(argument, &size);
  if (!source) return nullptr;
  if (size > 65536) {
    PyErr_SetString(PyExc_RuntimeError, "Expression source exceeds 65536 bytes");
    return nullptr;
  }
  try {
    std::lock_guard<std::mutex> guard(parserMutex);
    const std::string input(source, static_cast<std::size_t>(size));
    if (input.find('\0') != std::string::npos) {
      PyErr_SetString(PyExc_RuntimeError, "Expression source contains a null byte");
      return nullptr;
    }
    // Bound recursion before entering the recursive inspection parser. Quotes
    // and comments are skipped so literal punctuation does not consume depth.
    unsigned depth = 0;
    char quote = 0;
    bool comment = false, escaped = false;
    for (char ch : input) {
      if (comment) { if (ch == '\n') comment = false; continue; }
      if (quote) {
        if (escaped) escaped = false;
        else if (ch == '\\') escaped = true;
        else if (ch == quote) quote = 0;
        continue;
      }
      if (ch == '#') { comment = true; continue; }
      if (ch == '"' || ch == '\'') { quote = ch; continue; }
      if (ch == '(' || ch == '[' || ch == '{') {
        if (++depth > 128) {
          PyErr_SetString(PyExc_RuntimeError, "Expression inspection depth exceeded");
          return nullptr;
        }
      } else if ((ch == ')' || ch == ']' || ch == '}') && depth) --depth;
    }
    SeParser<ASTPolicy> parser(input);
    const auto root = parser.parse();
    if (!root) Py_RETURN_NONE;
    return describe(*root);
  } catch (const ParseError &error) {
    PyErr_SetString(PyExc_RuntimeError, error.what().c_str());
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "Expression inspection failed");
  }
  return nullptr;
}

PyMethodDef methods[] = {
    {"parse", parse, METH_O, "Parse source into an owned syntax tree."},
    {nullptr, nullptr, 0, nullptr}};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "_core", nullptr, -1, methods};
}  // namespace

PyMODINIT_FUNC PyInit__core() { return PyModule_Create(&module); }
