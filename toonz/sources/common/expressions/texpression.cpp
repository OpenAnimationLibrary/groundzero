

// TnzBase includes
#include "tgrammar.h"
#include "tparser.h"
#include "tunit.h"

#include "texpression.h"
#include "tseexpr.h"
#include "texception.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <QDebug>

using namespace TSyntax;

namespace {
const char seExprPrefix[] = "seexpr1:";

std::recursive_mutex &seExprGraphEvaluationGate() {
  // A malformed saved graph can contain cross-curve cycles. Acquiring this
  // gate before curve-owned locks prevents two workers from each holding one
  // side of that cycle while waiting for the other. Recursive acquisition lets
  // the existing same-thread cycle diagnostic reject the graph normally.
  static std::recursive_mutex gate;
  return gate;
}

struct ReferenceLiteral {
  std::size_t begin, end;
  char quote;
  std::string reference;
};

bool identifierCharacter(char c) {
  const unsigned char ch = static_cast<unsigned char>(c);
  return std::isalnum(ch) || c == '_';
}

void skipSpaceAndComments(const std::string &source, std::size_t &pos) {
  while (pos < source.size()) {
    if (std::isspace(static_cast<unsigned char>(source[pos]))) {
      ++pos;
    } else if (source[pos] == '#') {
      while (pos < source.size() && source[pos] != '\n') ++pos;
    } else {
      break;
    }
  }
}

bool readLiteral(const std::string &source, std::size_t &pos,
                 ReferenceLiteral &literal) {
  literal.quote = source[pos++];
  literal.begin = pos;
  literal.reference.clear();
  while (pos < source.size()) {
    char c = source[pos++];
    if (c == literal.quote) {
      literal.end = pos - 1;
      return true;
    }
    if (c == '\\' && pos < source.size()) {
      c = source[pos++];
      switch (c) {
      case 'n': literal.reference += '\n'; break;
      case 'r': literal.reference += '\r'; break;
      case 't': literal.reference += '\t'; break;
      case '\\': literal.reference += '\\'; break;
      case '"': literal.reference += '"'; break;
      default:
        // Match SeExpr's string escape convention, including single quotes.
        literal.reference += '\\';
        literal.reference += c;
      }
    } else {
      literal.reference += c;
    }
  }
  return false;
}

// References have literal names so dependencies are knowable before evaluation.
// A second, scalar frame argument remains a full SeExpr expression.
bool collectReferences(const std::string &source,
                       std::vector<ReferenceLiteral> &references,
                       std::string &error, int &errorOffset) {
  std::size_t pos = 0;
  while (pos < source.size()) {
    skipSpaceAndComments(source, pos);
    if (pos == source.size()) break;
    if (source[pos] == '"' || source[pos] == '\'') {
      ReferenceLiteral ignored;
      if (!readLiteral(source, pos, ignored)) break;  // Parser reports syntax.
      continue;
    }
    if (!identifierCharacter(source[pos])) {
      ++pos;
      continue;
    }
    const std::size_t begin = pos;
    while (pos < source.size() && identifierCharacter(source[pos])) ++pos;
    if (source.substr(begin, pos - begin) != "channel" ||
        (begin > 0 && source[begin - 1] == '$'))
      continue;
    std::size_t argument = pos;
    skipSpaceAndComments(source, argument);
    if (argument == source.size() || source[argument] != '(') continue;
    ++argument;
    skipSpaceAndComments(source, argument);
    errorOffset = static_cast<int>(argument);
    if (argument == source.size() ||
        (source[argument] != '"' && source[argument] != '\'')) {
      error = "channel() requires a literal native reference as its first argument";
      return false;
    }
    ReferenceLiteral literal;
    if (!readLiteral(source, argument, literal)) break;
    std::size_t after = argument;
    skipSpaceAndComments(source, after);
    if (after < source.size() && source[after] != ',' && source[after] != ')') {
      error = "channel() reference names cannot be computed dynamically";
      return false;
    }
    references.push_back(literal);
    pos = argument;
  }
  return true;
}

std::string quoteReference(const std::string &reference, char quote) {
  std::string result;
  for (char c : reference) {
    if (c == '\\' || c == quote) result += '\\';
    switch (c) {
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default: result += c; break;
    }
  }
  return result;
}

class SeExprCalculatorNode final : public CalculatorNode {
  const Grammar *m_grammar;
  std::vector<std::pair<std::string, std::unique_ptr<TExpression>>> m_references;
  mutable TSeExpr::Program m_program;
  mutable std::mutex m_mutex;
  mutable double m_vars[3] = {0.0, 1.0, 1.0};
  bool m_valid = false;
  std::string m_error;
  std::pair<int, int> m_errorPos{0, 0};

public:
  SeExprCalculatorNode(Calculator *calculator, const Grammar *grammar,
                       TDoubleParam *owner, const std::string &source)
      : CalculatorNode(calculator), m_grammar(grammar) {
    if (source.size() > 65536) {
      m_error = "SeExpr animation source exceeds the supported size limit";
      return;
    }
    std::vector<ReferenceLiteral> references;
    int errorOffset = 0;
    if (!collectReferences(source, references, m_error, errorOffset)) {
      m_errorPos = {errorOffset, errorOffset + 1};
      return;
    }
    for (const auto &literal : references) {
      if (m_references.size() >= 512) {
        m_error = "SeExpr animation has too many channel references";
        return;
      }
      const auto existing = std::find_if(
          m_references.begin(), m_references.end(),
          [&](const auto &item) { return item.first == literal.reference; });
      if (existing != m_references.end()) continue;
      int depth = 0;
      bool quoted = false;
      for (char ch : literal.reference) {
        if (ch == '"') quoted = !quoted;
        if (!quoted && ch == '(' && ++depth > 128) {
          m_error = "Native channel reference nesting exceeds the supported limit";
          return;
        }
        if (!quoted && ch == ')') --depth;
      }
      std::unique_ptr<TExpression> expression(new TExpression);
      expression->setGrammar(grammar);
      expression->setOwnerParameter(owner);
      expression->setText(literal.reference);
      if (TExpression::isSeExprText(literal.reference) || !expression->isValid()) {
        m_error = "Invalid native channel reference: " + expression->getError();
        m_errorPos = {static_cast<int>(literal.begin),
                      static_cast<int>(literal.end)};
        return;
      }
      m_references.emplace_back(literal.reference, std::move(expression));
    }
    TSeExpr::Function channel;
    channel.name = "channel";
    channel.documentation =
        "channel(\"col1.x\"[, frame]): evaluate a native channel reference at "
        "a one-based frame; the reference must be a literal string.";
    channel.resultType = TSeExpr::Type::Scalar;
    channel.argumentTypes = {TSeExpr::Type::String, TSeExpr::Type::Scalar};
    channel.minArgs = 1;
    channel.maxArgs = 2;
    channel.evaluate = [this](const std::vector<TSeExpr::Value> &arguments) {
      for (const auto &reference : m_references) {
        if (reference.first != arguments[0].text) continue;
        Calculator *calculator = reference.second->getCalculator();
        if (!calculator) throw std::runtime_error("Native reference is unavailable");
        calculator->setUnit(getCalculator()->getUnit());
        const double frame = arguments.size() == 2
                                 ? arguments[1].number[0]
                                 : m_vars[CalculatorNode::FRAME];
        const double relative = m_vars[CalculatorNode::RFRAME] +
                                frame - m_vars[CalculatorNode::FRAME];
        return TSeExpr::Value::scalar(calculator->compute(m_vars[0], frame, relative));
      }
      throw std::runtime_error("Unbound or dynamic native channel reference");
    };
    std::vector<TSeExpr::Variable> variables;
    for (const std::string &name : {"t", "f", "frame", "r", "rframe", "time", "fps", "pi"})
      variables.push_back({name, TSeExpr::Value::scalar(0.0)});
    TSeExpr::Options options;
    options.resultType = TSeExpr::Type::Scalar;
    m_valid = m_program.compile(source, variables, {channel}, options);
    if (!m_valid) {
      m_error = m_program.errorMessage();
      if (!m_program.errors().empty()) {
        m_errorPos = {m_program.errors().front().begin,
                      m_program.errors().front().end};
      }
    }
  }

  bool isValid() const { return m_valid; }
  std::string error() const { return m_error; }
  std::pair<int, int> errorPos() const { return m_errorPos; }

  double compute(double vars[3]) const override {
    std::lock_guard<std::recursive_mutex> graphLock(seExprGraphEvaluationGate());
    // Animation curves can be evaluated by several render workers. Serialize
    // this curve-owned interpreter; FX workers own their separate Programs.
    static thread_local std::vector<const SeExprCalculatorNode *> evaluating;
    if (std::find(evaluating.begin(), evaluating.end(), this) != evaluating.end())
      throw TException("Circular SeExpr channel evaluation");
    std::lock_guard<std::mutex> lock(m_mutex);
    struct Active {
      std::vector<const SeExprCalculatorNode *> &stack;
      Active(std::vector<const SeExprCalculatorNode *> &s,
             const SeExprCalculatorNode *node) : stack(s) { stack.push_back(node); }
      ~Active() { stack.pop_back(); }
    } active(evaluating, this);
    std::copy(vars, vars + 3, m_vars);
    const double fps = m_grammar ? m_grammar->getFrameRate() : 24.0;
    m_program.setScalar("t", vars[0]);
    m_program.setScalar("f", vars[1]);
    m_program.setScalar("frame", vars[1]);
    m_program.setScalar("r", vars[2]);
    m_program.setScalar("rframe", vars[2]);
    m_program.setScalar("time", (vars[1] - 1.0) / fps);
    m_program.setScalar("fps", fps);
    m_program.setScalar("pi", std::acos(-1.0));
    TSeExpr::Value result;
    if (!m_program.evaluate(result))
      throw TException("SeExpr animation: " + m_program.errorMessage());
    return result.number[0];
  }

  void accept(CalculatorNodeVisitor &visitor) override {
    for (auto &reference : m_references) reference.second->accept(visitor);
  }
  bool hasReference() const override { return !m_references.empty(); }
  void setOwnerParameter(TDoubleParam *owner) override {
    for (auto &reference : m_references) reference.second->setOwnerParameter(owner);
  }
};
}  // namespace

//**********************************************************************************
//    TExpression::Imp  definition
//**********************************************************************************

class TExpression::Imp {
public:
  const Grammar *m_grammar;  //!< (not owned) The expression's grammar
  TDoubleParam *m_param;     //!< (not owned) The expression's owner
  Calculator *m_calculator;  //!< (owned) Expression calculator object

  std::string m_text;  //!< Text content

  std::string m_error;  //!< Description of an error in the expression's text
  std::pair<int, int>
      m_errorPos;  //!< Position of the error in the expression's text

  bool m_isValid,       //!< Whether the expression is valid
      m_hasBeenParsed,  //!< Whether the expression has already been parsed
      m_hasReference;
  double m_lastValue = 0.0;
  bool m_hasLastValue = false;
  bool m_runtimeError = false;
  std::string m_reportedError;
  mutable std::recursive_mutex m_mutex;

public:
  Imp()
      : m_grammar(0)
      , m_param(0)
      , m_calculator(0)
      , m_errorPos(0, -1)
      , m_isValid(false)
      , m_hasBeenParsed(true)
      , m_hasReference(false) {}
  ~Imp() { delete m_calculator; }
};

//**********************************************************************************
//    TExpression  implementation
//**********************************************************************************

TExpression::TExpression() : m_imp(new Imp()) {}

//--------------------------------------------------------------------------

TExpression::~TExpression() {}

//--------------------------------------------------------------------------

TExpression::TExpression(const TExpression &src) : m_imp(new Imp()) {
  std::lock_guard<std::recursive_mutex> lock(src.m_imp->m_mutex);
  m_imp->m_grammar       = src.m_imp->m_grammar;
  m_imp->m_param         = src.m_imp->m_param;
  m_imp->m_text          = src.m_imp->m_text;
  m_imp->m_calculator    = 0;
  m_imp->m_isValid       = src.m_imp->m_isValid;
  m_imp->m_error         = src.m_imp->m_error;
  m_imp->m_errorPos      = src.m_imp->m_errorPos;
  m_imp->m_hasBeenParsed = false;
  m_imp->m_hasReference  = src.m_imp->m_hasReference;
}

//--------------------------------------------------------------------------

TExpression &TExpression::operator=(TExpression copy) {
  std::swap(copy.m_imp, m_imp);
  return *this;
}

//--------------------------------------------------------------------------

void TExpression::setGrammar(const TSyntax::Grammar *grammar) {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  m_imp->m_grammar       = grammar;
  m_imp->m_hasBeenParsed = false;
}

//--------------------------------------------------------------------------

const TSyntax::Grammar *TExpression::getGrammar() const {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  return m_imp->m_grammar;
}

//--------------------------------------------------------------------------

void TExpression::setText(std::string text) {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  if (m_imp->m_text != text || m_imp->m_hasReference) {
    m_imp->m_text = text;
    delete m_imp->m_calculator;
    m_imp->m_calculator    = 0;
    m_imp->m_isValid       = false;
    m_imp->m_hasReference  = false;
    m_imp->m_hasBeenParsed = false;
    m_imp->m_error         = "";
    m_imp->m_errorPos      = std::make_pair(0, -1);
    m_imp->m_runtimeError = false;
    m_imp->m_reportedError.clear();
    m_imp->m_hasLastValue = false;
  }
}

//--------------------------------------------------------------------------

std::string TExpression::getText() const {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  return m_imp->m_text;
}

bool TExpression::isSeExprText(const std::string &text) {
  return text.compare(0, sizeof(seExprPrefix) - 1, seExprPrefix) == 0;
}

std::string TExpression::seExprSource(const std::string &text) {
  return isSeExprText(text) ? text.substr(sizeof(seExprPrefix) - 1) : text;
}

std::string TExpression::makeSeExprText(const std::string &source) {
  return std::string(seExprPrefix) + source;
}

std::string TExpression::rewriteSeExprReferences(
    const std::string &text,
    const std::function<std::string(const std::string &)> &replace) {
  if (!isSeExprText(text)) return text;
  std::string source = seExprSource(text);
  std::vector<ReferenceLiteral> references;
  std::string error;
  int offset = 0;
  if (!collectReferences(source, references, error, offset)) return text;
  // Work backwards so byte offsets remain valid after replacement.
  for (auto it = references.rbegin(); it != references.rend(); ++it) {
    const std::string rewritten = replace(it->reference);
    if (rewritten != it->reference)
      source.replace(it->begin - 1, it->end - it->begin + 2,
                     "\"" + quoteReference(rewritten, '"') + "\"");
  }
  return makeSeExprText(source);
}

//--------------------------------------------------------------------------

// note: unit is used by the CycleNode node only
TSyntax::Calculator *TExpression::getCalculator() {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  if (!m_imp->m_hasBeenParsed) parse();
  if (m_imp->m_isValid && m_imp->m_calculator)
    return m_imp->m_calculator;
  else
    return 0;
}

bool TExpression::evaluate(double t, double frame, double relativeFrame,
                           const TUnit *unit, double &result) {
  std::lock_guard<std::recursive_mutex> graphLock(seExprGraphEvaluationGate());
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  Calculator *calculator = getCalculator();
  if (calculator) {
    try {
      calculator->setUnit(unit);
      const double value = calculator->compute(t, frame, relativeFrame);
      if (isSeExprText(m_imp->m_text) && !std::isfinite(value))
        throw TException("SeExpr animation result is not finite");
      result = value;
      m_imp->m_lastValue = value;
      m_imp->m_hasLastValue = true;
      if (m_imp->m_runtimeError) {
        m_imp->m_error.clear();
        m_imp->m_runtimeError = false;
      }
      m_imp->m_reportedError.clear();
      return true;
    } catch (const TException &error) {
      m_imp->m_error = QString::fromStdWString(error.getMessage()).toStdString();
    } catch (const std::exception &error) {
      m_imp->m_error = error.what();
    } catch (...) {
      m_imp->m_error = "SeExpr animation evaluation failed";
    }
    m_imp->m_runtimeError = true;
  }
  if (m_imp->m_hasLastValue) result = m_imp->m_lastValue;
  if (isSeExprText(m_imp->m_text)) TSeExpr::reportFailure();
  if (isSeExprText(m_imp->m_text) && m_imp->m_reportedError != m_imp->m_error) {
    m_imp->m_reportedError = m_imp->m_error;
    qWarning().noquote() << "SeExpr animation:" << QString::fromStdString(m_imp->m_error);
  }
  return false;
}

//--------------------------------------------------------------------------

bool TExpression::isValid() {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  if (!m_imp->m_hasBeenParsed) parse();
  return m_imp->m_isValid;
}

//--------------------------------------------------------------------------

std::string TExpression::getError() const {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  return m_imp->m_error;
}

//--------------------------------------------------------------------------

std::pair<int, int> TExpression::getErrorPos() const {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  return m_imp->m_errorPos;
}

//--------------------------------------------------------------------------

void TExpression::accept(TSyntax::CalculatorNodeVisitor &visitor) {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  if (!m_imp->m_hasBeenParsed) parse();
  if (m_imp->m_isValid && m_imp->m_calculator)
    m_imp->m_calculator->accept(visitor);
}

//--------------------------------------------------------------------------

void TExpression::parse() {
  delete m_imp->m_calculator;
  m_imp->m_calculator = 0;

  m_imp->m_errorPos     = std::make_pair(0, -1);
  m_imp->m_error        = std::string();
  m_imp->m_hasReference = false;
  m_imp->m_runtimeError = false;

  if (!m_imp->m_grammar) {
    m_imp->m_error   = "No grammar defined";
    m_imp->m_isValid = false;
  } else if (isSeExprText(m_imp->m_text)) {
    std::unique_ptr<Calculator> calculator(new Calculator);
    calculator->setOwnerParameter(m_imp->m_param);
    std::unique_ptr<SeExprCalculatorNode> node(new SeExprCalculatorNode(
        calculator.get(), m_imp->m_grammar, m_imp->m_param,
        seExprSource(m_imp->m_text)));
    m_imp->m_isValid = node->isValid();
    m_imp->m_hasReference = node->hasReference();
    if (m_imp->m_isValid) {
      calculator->setRootNode(node.release());
      m_imp->m_calculator = calculator.release();
    } else {
      m_imp->m_error = node->error();
      m_imp->m_errorPos = node->errorPos();
      m_imp->m_errorPos.first += sizeof(seExprPrefix) - 1;
      m_imp->m_errorPos.second += sizeof(seExprPrefix) - 1;
    }
  } else {
    Parser parser(m_imp->m_grammar);

    m_imp->m_calculator = parser.parse(m_imp->m_text);

    if (m_imp->m_calculator)
      m_imp->m_calculator->setOwnerParameter(m_imp->m_param);

    m_imp->m_isValid      = parser.isValid();
    m_imp->m_hasReference = parser.hasReference();

    if (!m_imp->m_isValid) {
      m_imp->m_error    = parser.getError();
      m_imp->m_errorPos = parser.getErrorPos();
    }
  }

  m_imp->m_hasBeenParsed = true;
}

//--------------------------------------------------------------------------

void TExpression::setOwnerParameter(TDoubleParam *param) {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  m_imp->m_param = param;

  if (m_imp->m_calculator) m_imp->m_calculator->setOwnerParameter(param);
}

//--------------------------------------------------------------------------

TDoubleParam *TExpression::getOwnerParameter() const {
  std::lock_guard<std::recursive_mutex> lock(m_imp->m_mutex);
  return m_imp->m_param;
}

//--------------------------------------------------------------------------

bool TExpression::isCycling() const {
  if (isSeExprText(getText())) return false;
  // TODO: this is a quick&dirty implementation to be replaced with a more
  // "semantic" one
  return getText().find("cycle") != std::string::npos;
}
