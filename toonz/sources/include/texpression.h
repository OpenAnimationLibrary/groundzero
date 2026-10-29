#pragma once

#ifndef TEXPRESSION_INCLUDED
#define TEXPRESSION_INCLUDED

#include <memory>
#include <functional>

// TnzCore includes
#include "tcommon.h"

#undef DVAPI
#undef DVVAR
#ifdef TNZBASE_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//========================================================

//    Forward declaration

class TUnit;
class TDoubleParam;

namespace TSyntax {
class Grammar;
class CalculatorNodeVisitor;
class Calculator;
}

//========================================================

//************************************************************************************
//    TExpression  declaration
//************************************************************************************

//! This class manages a generic expression.
/*!
  An expression is a sequence of characters and contains:

  \li a \e CalcNode that is a basic element of the expression that must be
  evaluated;
  \li a \e Grammar that is a sequence of string patterns that define operations
  on nodes;
  \li a \e Builder that is a sequence or a tree of calcnodes and operations on
  that;
*/
class DVAPI TExpression {
  class Imp;
  std::unique_ptr<Imp> m_imp;

public:
  TExpression();
  ~TExpression();

  TExpression(const TExpression &);
  TExpression &operator=(TExpression);

  //! Sets the expression's grammar - does \b not acquire ownership.
  void setGrammar(const TSyntax::Grammar *grammar);
  const TSyntax::Grammar *getGrammar() const;

  /*!
Sets the expression's text - a mix of mathematical expressions
and grammar-recognized patterns that is able to calculate the
expression's value at a given frame.
*/
  void setText(std::string text);
  std::string getText() const;

  // The explicit version marker keeps existing native expressions unchanged.
  // Saved keyframes use a separate versioned tag for these expressions.
  static bool isSeExprText(const std::string &text);
  static std::string seExprSource(const std::string &text);
  static std::string makeSeExprText(const std::string &source);
  // Rewrite only native reference literals passed to channel(), never other
  // SeExpr string data, comments, or user-defined variable names.
  static std::string rewriteSeExprReferences(
      const std::string &text,
      const std::function<std::string(const std::string &)> &replace);

  TSyntax::Calculator *getCalculator();

  // GUI-safe evaluation. Failed SeExpr drafts/loaded data keep their source
  // and diagnostics; result retains the last valid preview when available.
  // Batch hosts must treat false as a failed evaluation, not successful output.
  bool evaluate(double t, double frame, double relativeFrame, const TUnit *unit,
                double &result);

  /*!
          Returns whether the expression is valid according to actual
grammar syntax (i.e. it has at least one calculator node).
  */
  bool isValid();

  std::string getError() const;
  std::pair<int, int> getErrorPos() const;

  void accept(TSyntax::CalculatorNodeVisitor &visitor);

  void setOwnerParameter(TDoubleParam *param);
  TDoubleParam *getOwnerParameter() const;

  bool isCycling() const;

private:
  // The expression is parsed lazily - ie when a getters attempts to
  // evaluate the expression and it was not yet parsed
  void parse();
};

#endif
