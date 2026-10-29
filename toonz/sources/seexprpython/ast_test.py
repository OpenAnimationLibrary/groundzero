"""Exercise native syntax traversal and literal editing in CI."""
import gc
import contextlib
import io
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

printed = io.StringIO()
with contextlib.redirect_stdout(printed):
    result = expr.printTree(tree, '3+4')
lines = printed.getvalue().splitlines()
require(result is None and 'printTree' in expr.__all__ and
        any('BinaryOp +' in line for line in lines) and
        [line.split('Num ', 1)[1] for line in lines if 'Num ' in line] == ['3.0', '4.0'] and
        any('└─ ' in line for line in lines),
        "public tree printing lost node names, source order or tree structure")

source = '"é"+3'
tree = expr.AST(source)
numbers = expr.traverseFilter(tree.root(), lambda node, children:
                              node.type == expr.ASTType.Num)
edits = expr.Edits(source)
edits.addEdit(numbers[0], 9)
require(edits.makeNewString() == '"é"+9', "UTF-8 byte edits lost source text")
require(expr.getComment(expr.AST('3 # note').root(), '3 # note').strip() == 'note',
        "trailing comment inspection failed")
source = '$first=3; $label="#literal"; $first'
number = expr.traverseFilter(expr.AST(source).root(), lambda item, children:
                              item.type == expr.ASTType.Num)[0]
require(expr.getComment(number, source) == '',
        "a hash inside a later string was treated as a control annotation")

source = '# é😀\n$bias=3; $bias+curve($bias,0,0,1,1,1,1)'
call = expr.traverseFilter(expr.AST(source).root(), lambda item, children:
                            item.type == expr.ASTType.Call)[0]
require(source.encode('utf-8')[slice(*call.range)].decode('utf-8') == 'curve($bias,0,0,1,1,1,1)',
        "a later call range included preceding Unicode source")
source = '$tint=[2,-0.25,1e-12]; $tint'
vector = expr.traverseFilter(expr.AST(source).root(), lambda item, children:
                              item.type == expr.ASTType.Vec)[0]
require(source.encode('utf-8')[slice(*vector.range)].decode('utf-8') == '[2,-0.25,1e-12]',
        "a vector range omitted its closing bracket")
source = '$amount=--2; $amount'
unary = expr.traverseFilter(expr.AST(source).root(), lambda item, children:
                             item.type == expr.ASTType.UnaryOp)
require([source.encode('utf-8')[slice(*node.range)].decode('utf-8') for node in unary] == ['--2', '-2'],
        "nested unary ranges did not preserve their own operator and complete operand")

try:
    expr.AST('1 + (')
except RuntimeError:
    pass
else:
    raise AssertionError("invalid syntax unexpectedly parsed")

# These fit the source-size limit and do not depend on bracket nesting. A
# failure must leave the parser lock and its owning trees usable immediately.
for label, source in (
        ('flat addition', '1+' * 16000 + '1'),
        ('malformed flat addition', '1+' * 16000),
        ('right-associative power', '1^' * 16000 + '1'),
        ('nested ternary', '1?' * 6000 + '1' + ':1' * 6000)):
    require(len(source.encode('utf-8')) < 65536, 'depth fixture exceeded the source-size limit')
    try:
        expr.AST(source)
    except RuntimeError:
        pass
    else:
        raise AssertionError(label + ' bypassed inspection depth limits')
    require(expr.AST('3+4').isValid() and expr.evaluate('3+4', result_type='scalar') == 7,
            label + ' failure left parsing or evaluation unusable')

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
