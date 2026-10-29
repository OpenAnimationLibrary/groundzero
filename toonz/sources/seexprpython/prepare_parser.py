"""Prepare the licensed inspection parser in a private build directory."""
import argparse
from pathlib import Path
import shutil


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError("Pinned inspection parser compatibility anchor changed")
    return text.replace(before, after, 1)


def prepare(source, output):
    source = source / "src" / "SeExpr2" / "parser"
    output = output / "SeExpr2" / "parser"
    output.mkdir(parents=True, exist_ok=True)
    for name in ("ASTNode.h", "SeExprLex.h", "SeExprLex.cpp", "SeExprParse.h"):
        shutil.copyfile(source / name, output / name)

    node = (output / "ASTNode.h").read_text(encoding="utf-8")
    node = replace_once(node, "class ASTNode {\n  public:\n",
                        "class ASTNode {\n  public:\n    virtual ~ASTNode() = default;\n")
    node = replace_once(node, "#include <vector>\n",
                        "#include <stdexcept>\n#include <vector>\n")
    # Validate height while constructing every subtree, before a deeply nested
    # owning tree can exist. Parent links propagate growth of the parser's
    # right-associative power chain, whose descendants are extended in place.
    node = replace_once(node,
        "        a.sequence(std::forward<Args&&>(args)...);\n",
        "        a.sequence(std::forward<Args&&>(args)...);\n"
        "        for (auto &child : _children) {\n"
        "            reserveInspectionHeight(child->_inspectionHeight + 1);\n"
        "            child->_inspectionParent = this;\n"
        "        }\n")
    node = replace_once(node,
        "    ASTNode* addChild(std::unique_ptr<ASTNode> node) {\n"
        "        _children.push_back(std::move(node));\n",
        "    ASTNode* addChild(std::unique_ptr<ASTNode> node) {\n"
        "        reserveInspectionHeight(node->_inspectionHeight + 1);\n"
        "        node->_inspectionParent = this;\n"
        "        _children.push_back(std::move(node));\n")
    node = replace_once(node,
        "        _children.pop_back();\n        return ret;\n",
        "        _children.pop_back();\n"
        "        ret->_inspectionParent = nullptr;\n        return ret;\n")
    node = replace_once(node,
        "  private:\n    ASTType _type;\n",
        "  private:\n"
        "    unsigned _inspectionHeight = 1;\n"
        "    ASTNode *_inspectionParent = nullptr;\n"
        "    void reserveInspectionHeight(unsigned height) {\n"
        "        for (ASTNode *node = this; node; node = node->_inspectionParent) {\n"
        "            if (height < node->_inspectionHeight) height = node->_inspectionHeight;\n"
        "            if (height > 128)\n"
        '                throw std::runtime_error("Expression inspection tree depth exceeded");\n'
        "            node->_inspectionHeight = height++;\n"
        "        }\n"
        "    }\n"
        "    ASTType _type;\n")
    (output / "ASTNode.h").write_text(node, encoding="utf-8")

    parser = (output / "SeExprParse.h").read_text(encoding="utf-8")
    # Ternary and arrow recursion can grow before their parent nodes exist, so
    # tree-height checks alone cannot protect the recursive parser stack.
    parser = replace_once(parser,
        "  private:\n    Lexer lexer;\n",
        "  private:\n"
        "    unsigned _inspectionRecursion = 0;\n"
        "    struct InspectionRecursion {\n"
        "        SeParser &owner;\n"
        "        explicit InspectionRecursion(SeParser &parser) : owner(parser) {\n"
        "            if (owner._inspectionRecursion >= 128)\n"
        '                throw ParseError("Expression inspection parser recursion exceeded");\n'
        "            ++owner._inspectionRecursion;\n"
        "        }\n"
        "        ~InspectionRecursion() { --owner._inspectionRecursion; }\n"
        "    };\n"
        "    Lexer lexer;\n")
    for declaration in ("NodePtr expr() {", "NodePtr assignBlock(bool exprAtEnd) {",
                        "NodePtr ifthenelse() {"):
        parser = replace_once(parser, "    " + declaration + "\n",
                              "    " + declaration + "\n"
                              "        InspectionRecursion inspectionRecursion(*this);\n")
    # Ordinary calls begin at their name; only arrow calls include the input
    # expression before that name. The imported parser otherwise uses byte 0.
    parser = replace_once(parser,
        "int realStart = curriedArgument.get() != nullptr ? curriedArgument->range()[0] : 0;  // range.start;",
        "int realStart = curriedArgument.get() != nullptr ? curriedArgument->range()[0] : range.start()[0];")
    # Include the closing vector bracket in a replaceable literal's range.
    parser = replace_once(parser,
        '            vec->setRange(r());\n'
        '            ensureAndGetToken(token == Lexer::BRACKET_CLOSE, "Expected bracket close at end of vector");\n'
        '            return vec;',
        '            ensureAndGetToken(token == Lexer::BRACKET_CLOSE, "Expected bracket close at end of vector");\n'
        '            vec->setRange(r.toPrevious());\n'
        '            return vec;')
    # Construct unary chains from the operand outward, retaining each operator's
    # own byte start and the complete operand end. Evaluation order is unchanged.
    begin_marker = "    // expr8: {!|~|-|+}* expr9\n"
    end_marker = "    // expr9: {expr10 ^}* expr10\n"
    if parser.count(begin_marker) != 1 or parser.count(end_marker) != 1:
        raise RuntimeError("Pinned unary inspection parser compatibility anchor changed")
    begin = parser.index(begin_marker)
    end = parser.index(end_marker, begin)
    parser = parser[:begin] + '''    // expr8: {!|~|-|+}* expr9
    NodePtr expr8() {
        struct Unary { Range range; char op; };
        std::vector<Unary> operators;
        while (token == Lexer::TWIDLE || token == Lexer::NOT ||
               token == Lexer::MINUS || token == Lexer::PLUS) {
            const char op = token == Lexer::TWIDLE ? '~' :
                            token == Lexer::NOT ? '!' :
                            token == Lexer::MINUS ? '-' : '+';
            operators.push_back({tokenPosition, op});
            getToken();
        }
        NodePtr result = expr9();
        for (auto it = operators.rbegin(); it != operators.rend(); ++it) {
            const Range range{it->range[0], result->range()[1]};
            NodePtr parent(new typename Policy::UnaryOp(range, it->op));
            parent->addChild(std::move(result));
            result = std::move(parent);
        }
        return result;
    }

''' + parser[end:]
    (output / "SeExprParse.h").write_text(parser, encoding="utf-8")
    (output / "prepared.stamp").write_text("Inspection ownership, bounded depth and source-range compatibility\n", encoding="utf-8")


if __name__ == "__main__":
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument("--source", type=Path, required=True)
    argument_parser.add_argument("--output", type=Path, required=True)
    arguments = argument_parser.parse_args()
    prepare(arguments.source, arguments.output)
