"""Bound AST preparation and dispose rejected trees without recursive deletion."""

from pathlib import Path


def replace_once(text, before, after, label):
    if text.count(before) != 1:
        raise RuntimeError(f"Pinned SeExpr limits region no longer matches: {label}")
    return text.replace(before, after, 1)


def apply_limits(output):
    output = Path(output)
    header = output / "ExprNode.h"
    text = header.read_text(encoding="utf-8")
    for before, after in (
        ("const short int startPos, const short int endPos", "const int startPos, const int endPos"),
        ("inline short int startPos()", "inline int startPos()"),
        ("inline short int endPos()", "inline int endPos()"),
        ("inline short int length()", "inline int length()"),
        ("unsigned short int _startPos, _endPos;", "int _startPos, _endPos;"),
    ):
        text = replace_once(text, before, after, "source byte positions")
    header.write_text(text, encoding="utf-8", newline="\n")

    node = output / "ExprNode.cpp"
    text = node.read_text(encoding="utf-8")
    text = replace_once(text, """ExprNode::~ExprNode() {
    // delete children
    std::vector<ExprNode*>::iterator iter;
    for (iter = _children.begin(); iter != _children.end(); iter++) delete *iter;
}""", """ExprNode::~ExprNode() {
    // A source with a long flat arithmetic chain can have a very deep tree.
    // Keep derived destructors ahead of their children's destruction while
    // draining descendants iteratively, including parser-error cleanup.
    static thread_local std::vector<ExprNode*>* deleting = nullptr;
    if (deleting) {
        deleting->insert(deleting->end(), _children.begin(), _children.end());
        _children.clear();
        return;
    }
    std::vector<ExprNode*> pending;
    pending.swap(_children);
    deleting = &pending;
    while (!pending.empty()) {
        ExprNode* child = pending.back();
        pending.pop_back();
        delete child;
    }
    deleting = nullptr;
}""", "nonrecursive tree destruction")
    node.write_text(text, encoding="utf-8", newline="\n")

    expression = output / "Expression.cpp"
    text = expression.read_text(encoding="utf-8")
    text = replace_once(text, '#include "ExprWalker.h"',
                        '#include "ExprWalker.h"\n#include "seexprbudget.h"\n#include <utility>',
                        "tree limit dependencies")
    text = replace_once(text, """bool Expression::syntaxOK() const {
    parseIfNeeded();
    return _isValid;
}""", """bool Expression::syntaxOK() const {
    parseIfNeeded();
    return _parseTree != nullptr;
}""", "syntax query independent of type preparation")
    text = replace_once(text, """    if (!_parseTree) {
        addError(_parseError, tempStartPos, tempEndPos);
    }
}

void Expression::prep() const {""", """    if (_parseTree) {
        // Validate structural depth before recursive type preparation or
        // backend emission. Counting only parentheses misses flat operator
        // chains and repeated unary operators.
        try {
            std::vector<std::pair<const ExprNode*, size_t>> pending;
            pending.push_back({_parseTree, 1});
            while (!pending.empty()) {
                const auto item = pending.back();
                pending.pop_back();
                TSeExprInternal::checkAstDepth(item.second);
                TSeExprInternal::instructionStep();
                for (int index = 0; index < item.first->numChildren(); ++index)
                    pending.push_back({item.first->child(index), item.second + 1});
            }
        } catch (const std::exception& error) {
            _parseError = error.what();
            addError(_parseError, 0, static_cast<int>(_expression.size()));
            delete _parseTree;
            _parseTree = nullptr;
            return;
        }
    }
    if (!_parseTree) {
        addError(_parseError, tempStartPos, tempEndPos);
    }
}

void Expression::prep() const {""", "iterative depth validation before preparation")
    expression.write_text(text, encoding="utf-8", newline="\n")
