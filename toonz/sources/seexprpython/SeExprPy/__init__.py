"""SeExpr syntax inspection and editing for Python 3.

The AST interface follows the upstream SeExprPy inspection API. Ranges are
UTF-8 byte offsets, matching the native parser. No Python interpreter is
embedded into the application by this separately built module.
"""
from enum import IntEnum
import json
import math

from . import _core

evaluate = _core.evaluate
validate = _core.validate
functions = _core.functions
native_functions = _core.native_functions


class ASTType(IntEnum):
    Invalid = 0
    Module = 1
    Prototype = 2
    LocalFunction = 3
    Block = 4
    Node = 5
    IfThenElse = 6
    Vec = 7
    Cond = 8
    Compare = 9
    UnaryOp = 10
    BinaryOp = 11
    CompareEq = 12
    TernaryOp = 13
    Subscript = 14
    Var = 15
    Num = 16
    Call = 17
    Assign = 18
    String = 19
    Def = 20


class ASTHandle:
    def __init__(self, node):
        self._node = node

    @property
    def type(self):
        return ASTType(self._node["type"])

    @property
    def value(self):
        return self._node["value"]

    @property
    def range(self):
        return tuple(self._node["range"])

    def children(self):
        return [ASTHandle(child) for child in self._node["children"]]


class AST:
    def __init__(self, source):
        self._root = _core.parse(source)

    def isValid(self):
        return self._root is not None

    def root(self):
        return ASTHandle(self._root) if self._root is not None else None


def traverseCallback(node, callback):
    if node is None:
        return
    children = node.children()
    callback(node, children)
    for child in children:
        traverseCallback(child, callback)


def traverseFilter(node, filterCallback):
    found = []
    traverseCallback(node, lambda item, children:
                     found.append(item) if filterCallback(item, children) else None)
    return found


def printTree(tree, source=""):
    """Print the inspection tree, retaining the upstream ``(AST, source)`` API.

    The source argument remains accepted for existing inspection scripts. Node
    values and names come from the owned AST, so printing does not reparse or
    change the expression.
    """
    root = tree.root()
    if root is None:
        return
    pending = [(root, "   ", "", "   ")]
    while pending:
        node, prefix, branch, child_prefix = pending.pop()
        print(prefix + branch + node.type.name + " " + str(node.value))
        children = node.children()
        for index in range(len(children) - 1, -1, -1):
            last = index == len(children) - 1
            pending.append((children[index], child_prefix,
                            "└─ " if last else "├─ ",
                            child_prefix + ("   " if last else "│  ")))


def getComment(node, source):
    encoded = source.encode("utf-8")
    end = node.range[1]
    line_end = encoded.find(b"\n", end)
    tail = encoded[end:line_end if line_end != -1 else len(encoded)]
    quote = None
    escaped = False
    for index, character in enumerate(tail):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == ord("\\"):
                escaped = True
            elif character == quote:
                quote = None
        elif character in (ord('"'), ord("'")):
            quote = character
        elif character == ord("#"):
            return tail[index + 1:].decode("utf-8")
    return ""


class Edits:
    def __init__(self, source):
        self.str = source
        self.changes = []

    def addEdit(self, node, newValue):
        self.changes.append((node.range, newValue))

    def formatNewValue(self, value):
        if isinstance(value, str):
            return json.dumps(value, ensure_ascii=False)
        if isinstance(value, bool):
            return "1" if value else "0"
        if isinstance(value, (int, float)) and math.isfinite(value):
            return repr(value)
        raise ValueError("An edited literal must be a finite number or string")

    def makeNewString(self):
        data = self.str.encode("utf-8")
        ordered = sorted(self.changes, key=lambda change: change[0][0], reverse=True)
        previous_start = len(data)
        for (start, end), value in ordered:
            if not 0 <= start <= end <= previous_start:
                raise ValueError("Expression edits overlap or have invalid ranges")
            data = data[:start] + self.formatNewValue(value).encode("utf-8") + data[end:]
            previous_start = start
        return data.decode("utf-8")


__all__ = ["AST", "ASTHandle", "ASTType", "Edits", "getComment",
           "traverseCallback", "traverseFilter", "printTree", "evaluate",
           "validate", "functions", "native_functions"]
