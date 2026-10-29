"""Exercise native syntax traversal and literal editing in CI."""
import gc
import SeExprPy as expr


def require(condition, message):
    if not condition:
        raise AssertionError(message)


source = '$a=3; $a+map("fixture", 4)'
tree = expr.AST(source)
require(tree.isValid(), "valid formula did not parse")
calls = expr.traverseFilter(tree.root(), lambda node, children:
                            node.type == expr.ASTType.Call)
require(len(calls) == 1 and calls[0].value == "map", "call inspection failed")
del tree
gc.collect()
require(len(calls[0].children()) == 2, "child handle did not retain owned nodes")

tree = expr.AST('3+4')
numbers = expr.traverseFilter(tree.root(), lambda node, children:
                              node.type == expr.ASTType.Num)
edits = expr.Edits('3+4')
for node in numbers:
    edits.addEdit(node, node.value * 2)
require(edits.makeNewString() == '6.0+8.0', "literal editing failed")

source = '"é"+3'
tree = expr.AST(source)
numbers = expr.traverseFilter(tree.root(), lambda node, children:
                              node.type == expr.ASTType.Num)
edits = expr.Edits(source)
edits.addEdit(numbers[0], 9)
require(edits.makeNewString() == '"é"+9', "UTF-8 byte edits lost source text")
require(expr.getComment(expr.AST('3 # note').root(), '3 # note').strip() == 'note',
        "trailing comment inspection failed")

try:
    expr.AST('1 + (')
except RuntimeError:
    pass
else:
    raise AssertionError("invalid syntax unexpectedly parsed")

try:
    edits = expr.Edits('3')
    node = expr.traverseFilter(expr.AST('3').root(), lambda item, children:
                               item.type == expr.ASTType.Num)[0]
    edits.addEdit(node, 1)
    edits.addEdit(node, 2)
    edits.makeNewString()
except ValueError:
    pass
else:
    raise AssertionError("overlapping edits were not rejected")

print("SeExpr Python AST: PASS")
