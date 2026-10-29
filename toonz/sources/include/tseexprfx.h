#pragma once

#include "tseexpr.h"

#include <string>
#include <vector>

class TStringParam;
namespace TSyntax { class Grammar; }

// Native discovery interface; the editor never owns the effect or its state.
class TFxSeExprSource {
public:
  virtual ~TFxSeExprSource() = default;
  virtual TStringParam *seExprDocumentParam() const = 0;
  // Borrowed only while binding an editor's guarded, transient link resolver.
  // Rendering snapshots never retain or consult this native grammar pointer.
  virtual const TSyntax::Grammar *seExprGrammar() const { return nullptr; }
  virtual bool supportsSeExprSurfaceDocument() const { return false; }
  virtual bool validateSeExprDocument(
      const std::wstring &document, double frame, std::string &error,
      std::vector<TSeExpr::Error> *diagnostics = nullptr) const = 0;
  virtual std::vector<TSeExpr::Variable> seExprVariables(double frame) const = 0;
  virtual std::vector<TSeExpr::Function> seExprFunctions(double frame) const = 0;
  virtual bool previewSeExprDocument(const std::wstring &document, double frame,
                                     std::string &value, std::string &error) const = 0;
  virtual bool sampleSeExprDocument(const std::wstring &document, double frame,
                                     TSeExpr::Value &rgb, double &alpha,
                                     std::string &error) const = 0;
};
