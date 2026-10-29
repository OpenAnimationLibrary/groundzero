#pragma once

#include "tseexpr.h"

#include <string>
#include <vector>

class TStringParam;

// Native discovery interface; the editor never owns the effect or its state.
class TFxSeExprSource {
public:
  virtual ~TFxSeExprSource() = default;
  virtual TStringParam *seExprDocumentParam() const = 0;
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
