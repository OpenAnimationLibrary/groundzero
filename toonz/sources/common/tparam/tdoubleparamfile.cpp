

#include "tdoubleparamfile.h"
#include "tfilepath_io.h"
#include "tconvert.h"
#include <QFileInfo>
#include <cmath>

namespace {

//---------------------------------------------------------

bool parseDouble(double &value, char *&s, bool &valid) {
  if (!(*s == '-' || *s == '.' || ('0' <= *s && *s <= '9'))) return false;
  char *t = s;
  if (*s == '-') s++;
  while ('0' <= *s && *s <= '9') s++;
  if (*s == '.') {
    s++;
    while ('0' <= *s && *s <= '9') s++;
    if (*s == 'e' || *s == 'E') {
      s++;
      if (*s == '+' || *s == '-') s++;
      while ('0' <= *s && *s <= '9') s++;
    }
  }
  std::string w(t, s - t);
  size_t consumed = 0;
  value = std::stod(w, &consumed);
  if (consumed != w.size()) valid = false;
  return true;
}

//---------------------------------------------------------

int split(std::vector<double> &values, char *s, bool &valid) {
  int count = 0;
  for (;;) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || (*s == '\r' && s[1] == '\0')) break;
    double value = 0;
    bool found   = parseDouble(value, s, valid);
    if (!found) valid = false;
    if (found) {
      count++;
      values.push_back(value);
      while (*s == ' ' || *s == '\t') s++;
    }
    if (*s == ',' || *s == ';') {
      if (!found) {
        count++;
        values.push_back(0);
      }
      s++;
    } else {
      if (!found) break;
    }
  }
  return count;
}

//---------------------------------------------------------
}  // namespace
//---------------------------------------------------------

TDoubleParamFileData::TDoubleParamFileData() : m_params(), m_dirtyFlag(false) {}

//---------------------------------------------------------

TDoubleParamFileData::~TDoubleParamFileData() {}

//---------------------------------------------------------

void TDoubleParamFileData::setParams(
    const TDoubleKeyframe::FileParams &params) {
  m_params = params;
  invalidate();
}

//---------------------------------------------------------

void TDoubleParamFileData::invalidate() {
  m_dirtyFlag = true;
  m_values.clear();
  m_readError.clear();
}

//---------------------------------------------------------

double TDoubleParamFileData::getValue(double frame, double defaultValue) {
  if (frame < 0) return defaultValue;
  int intFrame = (int)frame;
  if (m_dirtyFlag) read();
  if (intFrame >= (int)m_values.size())
    return defaultValue;
  else
    return m_values[intFrame];
}

//---------------------------------------------------------

bool TDoubleParamFileData::getValueChecked(double frame, double defaultValue,
                                          double &value, std::string &error) {
  const QFileInfo file(m_params.m_path.getQString());
  if (!file.isFile() || !file.isReadable()) {
    error = "Animation data file is unavailable";
    return false;
  }
  if (m_dirtyFlag) read();
  error = m_readError;
  if (!error.empty()) return false;
  if (!std::isfinite(frame)) {
    error = "Animation data sample frame is not finite";
    return false;
  }
  // The native file curve deliberately uses its parameter default before/after
  // the table. That is valid extrapolation, unlike an unreadable or malformed
  // table. Keep it equivalent in checked sampling and avoid an overflowing cast.
  if (frame < 0 || frame >= static_cast<double>(m_values.size())) {
    value = defaultValue;
    return true;
  }
  value = m_values[static_cast<int>(frame)];
  return true;
}

//---------------------------------------------------------

void TDoubleParamFileData::read() {
  m_dirtyFlag = false;
  m_values.clear();
  m_readError.clear();
  int k = m_params.m_fieldIndex;
  if (k < 0) {
    m_readError = "Animation data field index is invalid";
    return;
  }
  TFilePath fp = m_params.m_path;
  Tifstream is(fp);
  if (!is) m_readError = "Animation data file is unavailable";
  char buffer[2048];
  memset(buffer, 0, sizeof buffer);
  while (is) {
    is.getline(buffer, sizeof(buffer) - 1);
    if (is.fail() && !is.eof()) m_readError = "Animation data row is too long or unreadable";
    std::vector<double> fields;
    QString line(buffer);
    if (line.size() == 0 || line.startsWith("#")) continue;
    bool valid = true;
    try {
      split(fields, buffer, valid);
    } catch (...) {
      m_readError = "Animation data contains an invalid number";
      throw;
    }
    if (!valid || k >= static_cast<int>(fields.size()))
      m_readError = "Animation data row is malformed or lacks the selected field";
    double value                      = 0;
    if (k < (int)fields.size()) value = fields[k];
    if (!std::isfinite(value)) m_readError = "Animation data value is not finite";
    m_values.push_back(value);
  }
  if (is.bad()) m_readError = "Animation data file could not be read completely";
  if (m_values.empty() && m_readError.empty())
    m_readError = "Animation data file contains no values";
}

//---------------------------------------------------------
