// SPDX-License-Identifier: BSD-3-Clause

#include "fuzzdriver.h"

// TnzCore includes
#include "tconvert.h"
#include "texception.h"

// Qt includes
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#endif

// Sanitizers install their own signal handlers and need a huge address
// space, so the driver leaves both alone when one is active.
#if defined(__SANITIZE_ADDRESS__)
#define OTFUZZ_SANITIZER
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) ||     \
    __has_feature(thread_sanitizer)
#define OTFUZZ_SANITIZER
#endif
#endif

namespace otfuzz {

//=============================================================================
// Seed building helpers
//-----------------------------------------------------------------------------

void Writer::u8(unsigned value, bool isField) {
  if (isField) addField(pos(), 1);
  m_data.push_back(uint8_t(value));
}

void Writer::u16(unsigned value, bool isField) {
  if (isField) addField(pos(), 2);
  m_data.push_back(0);
  m_data.push_back(0);
  patchU16(pos() - 2, value);
}

void Writer::u32(uint32_t value, bool isField) {
  if (isField) addField(pos(), 4);
  m_data.insert(m_data.end(), 4, 0);
  patchU32(pos() - 4, value);
}

void Writer::bytes(const void *data, size_t size) {
  const uint8_t *begin = static_cast<const uint8_t *>(data);
  m_data.insert(m_data.end(), begin, begin + size);
}

void Writer::text(const char *str) { bytes(str, std::strlen(str)); }

void Writer::zeros(size_t count) { m_data.insert(m_data.end(), count, 0); }

void Writer::f64(double value) {
  uint8_t buffer[sizeof(double)];
  std::memcpy(buffer, &value, sizeof(double));
  bytes(buffer, sizeof(double));
}

void Writer::patchU16(size_t offset, unsigned value) {
  for (int i = 0; i < 2; ++i) {
    int shift                  = m_bigEndian ? 8 * (1 - i) : 8 * i;
    m_data[offset + size_t(i)] = uint8_t(value >> shift);
  }
}

void Writer::patchU32(size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    int shift                  = m_bigEndian ? 8 * (3 - i) : 8 * i;
    m_data[offset + size_t(i)] = uint8_t(value >> shift);
  }
}

void Writer::addField(size_t offset, int size) {
  Field field = {offset, size, m_bigEndian};
  m_fields.push_back(field);
}

bool has(const Bytes &data, size_t offset, size_t count) {
  return offset <= data.size() && count <= data.size() - offset;
}

unsigned getU16(const Bytes &data, size_t offset, bool bigEndian) {
  if (!has(data, offset, 2)) return 0;
  return bigEndian ? (unsigned(data[offset]) << 8) | data[offset + 1]
                   : (unsigned(data[offset + 1]) << 8) | data[offset];
}

uint32_t getU32(const Bytes &data, size_t offset, bool bigEndian) {
  if (!has(data, offset, 4)) return 0;
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    int index = bigEndian ? i : 3 - i;
    value     = (value << 8) | data[offset + size_t(index)];
  }
  return value;
}

bool readFile(const QString &path, Bytes &data) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  QByteArray contents = file.readAll();
  data.assign(contents.constData(), contents.constData() + contents.size());
  return true;
}

//=============================================================================
// Driver
//-----------------------------------------------------------------------------

namespace {

const char kPrefix[]           = "@@otfuzz ";
const size_t kCasesPerChild    = 200;  // limits leaked memory and files
const int kDefaultTimeoutInSec = 20;

enum RunMode { SkipKnown, IncludeKnown, OnlyKnown };

enum Outcome { Pending, Loaded, Rejected, Known, Skipped, Failed };

struct Options {
  bool m_child  = false;
  size_t m_from = 0, m_count = size_t(-1);
  QString m_workDir;
  bool m_list = false, m_inProcess = false;
  std::string m_caseId, m_dumpId;
  QString m_dumpFile, m_seedsDir;
  QStringList m_corpusDirs;
  RunMode m_mode     = SkipKnown;
  int m_timeoutInSec = kDefaultTimeoutInSec;
};

struct Case {
  size_t m_seed;
  std::string m_mutation;
};

struct Plan {
  std::vector<Seed> m_seeds;
  std::vector<Case> m_cases;
};

struct Result {
  Outcome m_outcome;
  std::string m_detail;
};

//-----------------------------------------------------------------------------

uint64_t nextRandom(uint64_t &state) {
  // splitmix64: small, fast and the same on every platform.
  uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z          = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

uint64_t hashString(const std::string &str) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : str) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::vector<std::string> splitString(const std::string &str, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t end = str.find(separator, start);
    if (end == std::string::npos) {
      parts.push_back(str.substr(start));
      return parts;
    }
    parts.push_back(str.substr(start, end - start));
    start = end + 1;
  }
}

bool parseNumber(const std::string &str, uint64_t &value) {
  if (str.empty() || str.size() > 20) return false;
  value = 0;
  for (char c : str) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + uint64_t(c - '0');
  }
  return true;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string toHex(const std::string &bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string hex;
  for (unsigned char c : bytes) {
    hex += digits[c >> 4];
    hex += digits[c & 15];
  }
  return hex;
}

bool fromHex(const std::string &hex, Bytes &bytes) {
  if (hex.size() % 2) return false;
  bytes.clear();
  for (size_t i = 0; i < hex.size(); i += 2) {
    int high = hexDigit(hex[i]), low = hexDigit(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    bytes.push_back(uint8_t(high * 16 + low));
  }
  return true;
}

std::string num(uint64_t value) { return std::to_string(value); }

//-----------------------------------------------------------------------------

bool fieldFormat(const std::string &op, int &bytes, bool &bigEndian) {
  bigEndian = op == "u16be" || op == "u32be";
  if (op == "u8")
    bytes = 1;
  else if (op == "u16le" || op == "u16be")
    bytes = 2;
  else if (op == "u32le" || op == "u32be")
    bytes = 4;
  else
    return false;
  return true;
}

std::string fieldOp(const Field &field) {
  if (field.m_bytes == 1) return "u8";
  return (field.m_bytes == 2 ? "u16" : "u32") +
         std::string(field.m_bigEndian ? "be" : "le");
}

uint64_t readField(const Bytes &data, size_t offset, int bytes,
                   bool bigEndian) {
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) {
    int index = bigEndian ? i : bytes - 1 - i;
    value     = (value << 8) | data[offset + size_t(index)];
  }
  return value;
}

void writeField(Bytes &data, size_t offset, int bytes, bool bigEndian,
                uint64_t value) {
  for (int i = 0; i < bytes; ++i) {
    int shift                = bigEndian ? 8 * (bytes - 1 - i) : 8 * i;
    data[offset + size_t(i)] = uint8_t(value >> shift);
  }
}

//! Applies a mutation spec (the part of a case id after '@') to the data.
//! The spec holds everything needed, so that a case id reproduces the input.
bool applyMutation(const std::string &spec, Bytes &data) {
  std::vector<std::string> p = splitString(spec, ':');
  const std::string &op      = p[0];
  uint64_t a = 0, b = 0;
  const size_t size = data.size();
  auto numbers      = [&](size_t count) {
    return p.size() == count + 1 && parseNumber(p[1], a) &&
           (count < 2 || parseNumber(p[2], b));
  };
  auto at = [&](uint64_t offset) {
    return data.begin() + std::ptrdiff_t(offset);
  };

  int bytes;
  bool bigEndian;
  if (op == "orig") return p.size() == 1;
  if (op == "trunc") {
    if (!numbers(1)) return false;
    if (a < size) data.resize(size_t(a));
  } else if (op == "flip") {
    if (!numbers(2) || b > 7) return false;
    if (a < size) data[size_t(a)] ^= uint8_t(1u << b);
  } else if (op == "set") {
    if (!numbers(2) || b > 255) return false;
    if (a < size) data[size_t(a)] = uint8_t(b);
  } else if (op == "rnd") {
    if (!numbers(2)) return false;
    uint64_t state = a;
    for (uint64_t i = 0; i < b && size > 0; ++i) {
      size_t offset = size_t(nextRandom(state) % size);
      data[offset]  = uint8_t(nextRandom(state));
    }
  } else if (fieldFormat(op, bytes, bigEndian)) {
    if (!numbers(2)) return false;
    if (a + uint64_t(bytes) <= size)
      writeField(data, size_t(a), bytes, bigEndian, b);
  } else if (op == "del") {
    if (!numbers(2)) return false;
    if (a < size) data.erase(at(a), at(std::min<uint64_t>(size, a + b)));
  } else if (op == "dup") {
    if (!numbers(2)) return false;
    if (a < size) {
      uint64_t end = std::min<uint64_t>(size, a + b);
      Bytes copy(at(a), at(end));
      data.insert(at(end), copy.begin(), copy.end());
    }
  } else if (op == "ins" || op == "rep") {
    bool replace = op == "rep";
    Bytes insert;
    if (p.size() != (replace ? 4u : 3u) || !parseNumber(p[1], a) ||
        (replace && !parseNumber(p[2], b)) || !fromHex(p.back(), insert))
      return false;
    if (a <= size) {
      if (replace) data.erase(at(a), at(std::min<uint64_t>(size, a + b)));
      data.insert(at(a), insert.begin(), insert.end());
    }
  } else
    return false;
  return true;
}

//! The fixed list of corruptions of one seed.
std::vector<std::string> makeMutations(const Seed &seed, bool textual) {
  const Bytes &data = seed.m_data;
  const size_t n    = data.size();
  std::vector<std::string> specs;
  specs.push_back("orig");

  // Truncations: every length in the header area, then spread over the file.
  std::set<size_t> lengths;
  for (size_t length = 0; length < n && length <= 48; ++length)
    lengths.insert(length);
  for (size_t i = 1; i < 25; ++i) lengths.insert(n * i / 25);
  for (size_t back = 1; back <= 4 && back <= n; ++back)
    lengths.insert(n - back);
  for (size_t length : lengths)
    if (length < n) specs.push_back("trunc:" + num(length));
  if (n == 0) return specs;

  uint64_t state = hashString(seed.m_name);
  auto offset    = [&]() { return num(nextRandom(state) % n); };

  // Bit flips: the low and high bit of every header byte, then at random.
  for (size_t o = 0; o < n && o < 32; ++o) {
    specs.push_back("flip:" + num(o) + ":0");
    specs.push_back("flip:" + num(o) + ":7");
  }
  for (int i = 0; i < 96; ++i) {
    std::string o = offset();
    specs.push_back("flip:" + o + ":" + num(nextRandom(state) % 8));
  }

  // Single byte overwrites with boundary values, and multi-byte damage.
  static const unsigned byteValues[] = {0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff};
  for (int i = 0; i < 48; ++i) {
    std::string o = offset();
    unsigned value =
        i % 8 < 6 ? byteValues[i % 8] : unsigned(nextRandom(state) & 0xff);
    specs.push_back("set:" + o + ":" + num(value));
  }
  static const unsigned counts[] = {2, 4, 8, 16};
  for (int i = 0; i < 24; ++i)
    specs.push_back("rnd:" + num(nextRandom(state)) + ":" + num(counts[i % 4]));

  // Length, count and offset fields set to boundary values.
  size_t fieldCount = std::min<size_t>(seed.m_fields.size(), 40);
  for (size_t i = 0; i < fieldCount; ++i) {
    const Field &field = seed.m_fields[i];
    if (!has(data, field.m_offset, size_t(field.m_bytes))) continue;
    uint64_t max = (uint64_t(1) << (8 * field.m_bytes)) - 1;
    uint64_t orig =
        readField(data, field.m_offset, field.m_bytes, field.m_bigEndian);
    const uint64_t values[] = {0,          1,   orig - 1, orig + 1,
                               orig * 2,   max, max >> 1, (max >> 1) + 1,
                               uint64_t(n)};
    std::set<uint64_t> seen;
    for (uint64_t value : values) {
      value &= max;
      if (value == orig || !seen.insert(value).second) continue;
      specs.push_back(fieldOp(field) + ":" + num(field.m_offset) + ":" +
                      num(value));
    }
  }

  if (textual) {
    for (int i = 0; i < 32; ++i) {
      std::string o = offset();
      specs.push_back("del:" + o + ":" + num(1 + nextRandom(state) % 8));
    }
    static const char *const tokens[] = {
        "<",   ">",   "/",    "\"",   "'",       "\\", "=",           "<!--",
        "-->", "<a>", "</a>", "<a/>", " id='1'", "-1", "99999999999", "\xff"};
    for (const char *token : tokens)
      for (int k = 0; k < 2; ++k)
        specs.push_back("ins:" + num(nextRandom(state) % (n + 1)) + ":" +
                        toHex(token));
    specs.push_back("ins:" + num(n / 2) + ":00");
    for (int i = 0; i < 16; ++i) {
      std::string o = offset();
      specs.push_back("dup:" + o + ":" + num(1 + nextRandom(state) % 32));
    }

    // Numbers in the text replaced with out-of-range values.
    std::vector<std::pair<size_t, size_t>> numbers;
    for (size_t i = 0; i < n;) {
      if (data[i] < '0' || data[i] > '9') {
        ++i;
        continue;
      }
      size_t start = (i > 0 && data[i - 1] == '-') ? i - 1 : i;
      while (i < n && data[i] >= '0' && data[i] <= '9') ++i;
      numbers.push_back(std::make_pair(start, i - start));
    }
    static const char *const replacements[] = {
        "-1", "2147483648", "99999999999999999999", "", "1e308"};
    size_t step = numbers.size() / 24 + 1;
    for (size_t k = 0; k < numbers.size(); k += step)
      for (const char *replacement : replacements)
        specs.push_back("rep:" + num(numbers[k].first) + ":" +
                        num(numbers[k].second) + ":" + toHex(replacement));
  }

  // Remove duplicates, keeping the order.
  std::set<std::string> seen;
  std::vector<std::string> unique;
  for (const std::string &spec : specs)
    if (seen.insert(spec).second) unique.push_back(spec);
  return unique;
}

//-----------------------------------------------------------------------------

bool writeFile(const QString &path, const Bytes &data) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  qint64 written = data.empty()
                       ? 0
                       : file.write(reinterpret_cast<const char *>(data.data()),
                                    qint64(data.size()));
  file.close();
  return written == qint64(data.size());
}

std::string safeName(const QString &name) {
  std::string result = name.toStdString();
  for (char &c : result)
    if (c == '@' || c == ' ' || c == '\'' || c == '"' || c == '/' || c == '\\')
      c = '_';
  return result;
}

bool buildPlan(const Harness &harness, const Options &options,
               const QString &seedsDir, Plan &plan, std::string &error) {
  try {
    plan.m_seeds = harness.m_makeSeeds(seedsDir);
  } catch (const std::exception &e) {
    error = std::string("building the seeds failed: ") + e.what();
    return false;
  } catch (...) {
    error = "building the seeds failed with an exception";
    return false;
  }

  for (const QString &dirPath : options.m_corpusDirs) {
    QDir dir(dirPath);
    if (!dir.exists()) {
      error = "corpus directory not found: " + dirPath.toStdString();
      return false;
    }
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files, QDir::Name)) {
      Seed seed;
      seed.m_name = "corpus_" + safeName(info.fileName());
      if (!readFile(info.filePath(), seed.m_data)) {
        error = "cannot read " + info.filePath().toStdString();
        return false;
      }
      plan.m_seeds.push_back(seed);
    }
  }

  std::set<std::string> names;
  for (size_t i = 0; i < plan.m_seeds.size(); ++i) {
    const Seed &seed = plan.m_seeds[i];
    if (seed.m_name.find('@') != std::string::npos ||
        !names.insert(seed.m_name).second) {
      error = "bad or duplicate seed name: " + seed.m_name;
      return false;
    }
    for (const std::string &spec : makeMutations(seed, harness.m_textual)) {
      Case c = {i, spec};
      plan.m_cases.push_back(c);
    }
  }
  return true;
}

std::string caseId(const Plan &plan, size_t index) {
  const Case &c = plan.m_cases[index];
  return plan.m_seeds[c.m_seed].m_name + "@" + c.m_mutation;
}

bool findCase(const Plan &plan, const std::string &id, size_t &index) {
  for (index = 0; index < plan.m_cases.size(); ++index)
    if (caseId(plan, index) == id) return true;
  return false;
}

Bytes caseData(const Plan &plan, size_t index) {
  const Case &c = plan.m_cases[index];
  Bytes data    = plan.m_seeds[c.m_seed].m_data;
  applyMutation(c.m_mutation, data);
  return data;
}

Result runInput(const Harness &harness, const Bytes &data,
                const QString &inputPath, RunMode mode) {
  const KnownIssue *issue =
      harness.m_knownIssue ? harness.m_knownIssue(data) : nullptr;
  if (mode == SkipKnown && issue) return {Known, issue->m_id};
  if (mode == OnlyKnown && !issue) return {Skipped, std::string()};
  if (!writeFile(inputPath, data))
    return {Failed, "the driver cannot write " + inputPath.toStdString()};
  try {
    harness.m_load(inputPath);
    return {Loaded, std::string()};
  } catch (const TException &e) {
    return {Rejected, ::to_string(e.getMessage())};
  } catch (const std::exception &e) {
    return {Rejected, e.what()};
  } catch (...) {
    return {Rejected, "exception"};
  }
}

//-----------------------------------------------------------------------------
// Child process side

void protocol(const std::string &line) {
  std::string text = "\n" + std::string(kPrefix) + line + "\n";
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

void crashHandler(int signalNumber) {
  // Only async-signal-safe calls here.
  char buffer[64];
  size_t length = 0;
  for (const char *p = "\n@@otfuzz signal "; *p; ++p) buffer[length++] = *p;
  char digits[12];
  int count      = 0;
  unsigned value = signalNumber > 0 ? unsigned(signalNumber) : 0u;
  do {
    digits[count++] = char('0' + value % 10);
    value /= 10;
  } while (value && count < 11);
  while (count) buffer[length++] = digits[--count];
  buffer[length++] = '\n';
#ifdef _WIN32
  _write(1, buffer, unsigned(length));
#else
  ssize_t written = write(1, buffer, length);
  (void)written;
#endif
  std::signal(signalNumber, SIG_DFL);
  std::raise(signalNumber);
}

void installCrashHandlers() {
#ifndef OTFUZZ_SANITIZER
  std::signal(SIGSEGV, crashHandler);
  std::signal(SIGFPE, crashHandler);
  std::signal(SIGILL, crashHandler);
  std::signal(SIGABRT, crashHandler);
#ifdef SIGBUS
  std::signal(SIGBUS, crashHandler);
#endif
#endif
}

void limitMemory() {
#if defined(__linux__) && !defined(OTFUZZ_SANITIZER)
  // A corrupted size field must fail an allocation rather than make the
  // machine swap or the OOM killer stop the wrong process.
  const rlim_t limit = rlim_t(3) << 30;
  struct rlimit current;
  if (getrlimit(RLIMIT_AS, &current) == 0 &&
      (current.rlim_max == RLIM_INFINITY || current.rlim_max > limit)) {
    current.rlim_cur = limit;
    setrlimit(RLIMIT_AS, &current);
  }
#endif
}

const char *outcomeName(Outcome outcome) {
  switch (outcome) {
  case Loaded:
    return "loaded";
  case Rejected:
    return "rejected";
  case Known:
    return "known";
  case Skipped:
    return "skipped";
  case Failed:
    return "failed";
  default:
    return "pending";
  }
}

int runChild(const Harness &harness, const Options &options, const Plan &plan,
             const QString &inputPath) {
  installCrashHandlers();
  limitMemory();
  protocol("plan " + num(plan.m_cases.size()));
  size_t end = plan.m_cases.size();
  if (options.m_count < end - std::min(end, options.m_from))
    end = options.m_from + options.m_count;
  for (size_t i = options.m_from; i < end; ++i) {
    protocol("begin " + num(i));
    Result result =
        runInput(harness, caseData(plan, i), inputPath, options.m_mode);
    std::string line = "end " + num(i) + " " + outcomeName(result.m_outcome);
    if (result.m_outcome == Known) line += " " + result.m_detail;
    protocol(line);
  }
  protocol("done " + num(end));
  return 0;
}

//-----------------------------------------------------------------------------
// Report

struct Report {
  std::vector<Outcome> m_outcomes;
  std::vector<std::string> m_known;  // known issue id per case
  std::vector<std::pair<size_t, std::string>> m_failures;
};

std::string signalName(int signalNumber) {
  const char *name = nullptr;
  if (signalNumber == SIGSEGV) name = "SIGSEGV";
  if (signalNumber == SIGFPE) name = "SIGFPE";
  if (signalNumber == SIGILL) name = "SIGILL";
  if (signalNumber == SIGABRT) name = "SIGABRT";
#ifdef SIGBUS
  if (signalNumber == SIGBUS) name = "SIGBUS";
#endif
  return "signal " + std::to_string(signalNumber) +
         (name ? std::string(" ") + name : std::string());
}

int printReport(const Harness &harness, const Options &options,
                const Plan &plan, const Report &report) {
  std::map<Outcome, size_t> counts;
  std::map<std::string, std::pair<size_t, size_t>> known;  // count, first
  for (size_t i = 0; i < report.m_outcomes.size(); ++i) {
    ++counts[report.m_outcomes[i]];
    if (report.m_outcomes[i] != Known) continue;
    auto it = known.find(report.m_known[i]);
    if (it == known.end())
      known[report.m_known[i]] = std::make_pair(size_t(1), i);
    else
      ++it->second.first;
  }

  std::printf(
      "%s: %zu cases from %zu seeds: %zu loaded, %zu rejected by the loader, "
      "%zu skipped as known issues, %zu not selected, %zu FAILED\n",
      harness.m_name, plan.m_cases.size(), plan.m_seeds.size(), counts[Loaded],
      counts[Rejected], counts[Known], counts[Skipped],
      report.m_failures.size());
  for (const auto &entry : known)
    std::printf("  known issue %s: %zu cases, e.g. %s\n", entry.first.c_str(),
                entry.second.first, caseId(plan, entry.second.second).c_str());

  const std::string exe = QFileInfo(QCoreApplication::applicationFilePath())
                              .fileName()
                              .toStdString();
  for (const auto &failure : report.m_failures) {
    const std::string id = caseId(plan, failure.first);
    std::printf("FAILED %s: %s\n", id.c_str(), failure.second.c_str());
    std::printf("  reproduce: %s --case '%s'\n", exe.c_str(), id.c_str());
    std::printf("  input:     %s --dump '%s' input.%s\n", exe.c_str(),
                id.c_str(), harness.m_extension);
  }
  if (!report.m_failures.empty() && options.m_mode == SkipKnown)
    std::printf(
        "%s: inputs above crashed or hung the loader. Fix the loader, or add a "
        "KnownIssue to %s_fuzz.cpp if the fix has to wait.\n",
        harness.m_name, harness.m_name);
  std::fflush(stdout);
  return report.m_failures.empty() ? 0 : 1;
}

//-----------------------------------------------------------------------------
// Supervisor side

QStringList childArguments(const Options &options, const QString &workDir) {
  QStringList args;
  for (const QString &dir : options.m_corpusDirs) args << "--corpus" << dir;
  if (options.m_mode == IncludeKnown) args << "--include-known";
  if (options.m_mode == OnlyKnown) args << "--only-known";
  args << "--work-dir" << workDir;
  return args;
}

int supervise(const Harness &harness, const Options &options, const Plan &plan,
              const QString &workDir) {
  const size_t total = plan.m_cases.size();
  Report report;
  report.m_outcomes.assign(total, Pending);
  report.m_known.assign(total, std::string());
  const std::string prefix(kPrefix);

  size_t next = 0;
  while (next < total) {
    const size_t start = next;
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QStringList args = childArguments(options, workDir);
    args << "--child-from" << QString::number(qulonglong(start))
         << "--child-count" << QString::number(qulonglong(kCasesPerChild));
    child.start(QCoreApplication::applicationFilePath(), args);
    if (!child.waitForStarted(60000)) {
      std::fprintf(stderr, "%s: cannot start a child process\n",
                   harness.m_name);
      return 2;
    }

    std::string pending;
    long long current = -1;
    bool done = false, timedOut = false;
    size_t doneAt    = 0;
    int signalNumber = 0;
    QElapsedTimer timer;
    timer.start();
    for (;;) {
      const bool finished = child.state() == QProcess::NotRunning;
      if (!finished) child.waitForReadyRead(200);
      pending += child.readAllStandardOutput().toStdString();

      size_t eol;
      while ((eol = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, eol);
        pending.erase(0, eol + 1);
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::vector<std::string> words =
            splitString(line.substr(prefix.size()), ' ');
        uint64_t index = 0;
        if (words.size() < 2 || !parseNumber(words[1], index)) continue;
        if (words[0] == "plan" && index != total) {
          std::fprintf(stderr,
                       "%s: the child built %llu cases instead of %zu; the "
                       "seed builders are not deterministic\n",
                       harness.m_name, (unsigned long long)index, total);
          child.kill();
          child.waitForFinished(10000);
          return 2;
        } else if (words[0] == "begin" && index < total) {
          current = (long long)index;
          timer.restart();
        } else if (words[0] == "end" && words.size() >= 3 && index < total) {
          Outcome outcome = Failed;
          for (Outcome o : {Loaded, Rejected, Known, Skipped})
            if (words[2] == outcomeName(o)) outcome = o;
          report.m_outcomes[index] = outcome;
          if (outcome == Failed)
            report.m_failures.push_back(
                std::make_pair(size_t(index), std::string("driver error")));
          if (words.size() >= 4) report.m_known[index] = words[3];
          current = -1;
          next    = size_t(index) + 1;
        } else if (words[0] == "signal")
          signalNumber = int(index);
        else if (words[0] == "done") {
          done   = true;
          doneAt = size_t(index);
        }
      }
      if (finished) break;
      if (current >= 0 && timer.elapsed() > options.m_timeoutInSec * 1000LL) {
        timedOut = true;
        child.kill();
        child.waitForFinished(10000);
      }
    }

    if (done) {
      next = std::max(next, doneAt);
    } else if (current >= 0) {
      std::string what;
      if (timedOut)
        what = "hang (no result after " +
               std::to_string(options.m_timeoutInSec) + " s)";
      else if (signalNumber)
        what = "crash (" + signalName(signalNumber) + ")";
      else if (child.exitStatus() == QProcess::CrashExit)
        what = "crash";
      else
        what = "process exited with code " + std::to_string(child.exitCode()) +
               " (sanitizer report or abort above?)";
      report.m_outcomes[size_t(current)] = Failed;
      report.m_failures.push_back(std::make_pair(size_t(current), what));
      next = size_t(current) + 1;
    } else if (next == start) {
      std::fprintf(stderr,
                   "%s: a child process stopped before running case %zu "
                   "(exit code %d)\n",
                   harness.m_name, start, child.exitCode());
      return 2;
    }
  }
  return printReport(harness, options, plan, report);
}

int runInProcess(const Harness &harness, const Options &options,
                 const Plan &plan, const QString &inputPath) {
  Report report;
  for (size_t i = 0; i < plan.m_cases.size(); ++i) {
    Result result =
        runInput(harness, caseData(plan, i), inputPath, options.m_mode);
    report.m_outcomes.push_back(result.m_outcome);
    report.m_known.push_back(result.m_outcome == Known ? result.m_detail
                                                       : std::string());
    if (result.m_outcome == Failed)
      report.m_failures.push_back(std::make_pair(i, result.m_detail));
  }
  return printReport(harness, options, plan, report);
}

int runOneCase(const Harness &harness, const Plan &plan, const std::string &id,
               const QString &inputPath) {
  size_t index;
  if (!findCase(plan, id, index)) {
    std::fprintf(stderr, "%s: no case '%s' (see --list)\n", harness.m_name,
                 id.c_str());
    return 2;
  }
  Bytes data = caseData(plan, index);
  const KnownIssue *issue =
      harness.m_knownIssue ? harness.m_knownIssue(data) : nullptr;
  if (issue)
    std::printf("%s: this input reaches known issue %s: %s\n", id.c_str(),
                issue->m_id, issue->m_summary);
  std::fflush(stdout);
  Result result = runInput(harness, data, inputPath, IncludeKnown);
  std::printf("%s: %s%s%s\n", id.c_str(), outcomeName(result.m_outcome),
              result.m_detail.empty() ? "" : ": ", result.m_detail.c_str());
  return result.m_outcome == Failed ? 1 : 0;
}

//-----------------------------------------------------------------------------

bool parseOptions(int argc, char *argv[], Options &options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value            = [&](std::string &out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string v, w;
    uint64_t number = 0;
    if (arg == "--list")
      options.m_list = true;
    else if (arg == "--in-process")
      options.m_inProcess = true;
    else if (arg == "--include-known")
      options.m_mode = IncludeKnown;
    else if (arg == "--only-known")
      options.m_mode = OnlyKnown;
    else if (arg == "--case" && value(v))
      options.m_caseId = v;
    else if (arg == "--dump" && value(v) && value(w)) {
      options.m_dumpId   = v;
      options.m_dumpFile = QString::fromLocal8Bit(w.c_str());
    } else if (arg == "--write-seeds" && value(v))
      options.m_seedsDir = QString::fromLocal8Bit(v.c_str());
    else if (arg == "--corpus" && value(v))
      options.m_corpusDirs << QString::fromLocal8Bit(v.c_str());
    else if (arg == "--timeout" && value(v) && parseNumber(v, number) &&
             number > 0 && number < 100000)
      options.m_timeoutInSec = int(number);
    else if (arg == "--work-dir" && value(v))
      options.m_workDir = QString::fromLocal8Bit(v.c_str());
    else if (arg == "--child-from" && value(v) && parseNumber(v, number)) {
      options.m_child = true;
      options.m_from  = size_t(number);
    } else if (arg == "--child-count" && value(v) && parseNumber(v, number))
      options.m_count = size_t(number);
    else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

}  // namespace

//=============================================================================

int runDriver(const Harness &harness, int argc, char *argv[]) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: %s [--list | --case <id> | --dump <id> <file> | "
                 "--write-seeds <dir>] [--corpus <dir>]... [--include-known | "
                 "--only-known] [--in-process] [--timeout <seconds>]\n",
                 argc > 0 ? argv[0] : "fuzz_test");
    return 2;
  }

  // The supervisor's temporary directory also holds the children's files,
  // so nothing is left behind when a child crashes.
  QTemporaryDir temporary;
  QString workDir = options.m_workDir;
  if (workDir.isEmpty()) {
    if (!temporary.isValid()) {
      std::fprintf(stderr, "%s: cannot create a temporary directory\n",
                   harness.m_name);
      return 2;
    }
    workDir = temporary.path();
  }
  const QString seedsDir  = workDir + "/seeds";
  const QString inputDir  = workDir + "/input";
  const QString inputPath = inputDir + "/fuzz." + harness.m_extension;
  QDir(seedsDir).removeRecursively();
  QDir(inputDir).removeRecursively();
  if (!QDir().mkpath(seedsDir) || !QDir().mkpath(inputDir)) {
    std::fprintf(stderr, "%s: cannot create %s\n", harness.m_name,
                 workDir.toLocal8Bit().constData());
    return 2;
  }

  Plan plan;
  std::string error;
  if (!buildPlan(harness, options, seedsDir, plan, error)) {
    std::fprintf(stderr, "%s: %s\n", harness.m_name, error.c_str());
    return 2;
  }

  if (options.m_child) {
    int code = runChild(harness, options, plan, inputPath);
    std::fflush(stdout);
    std::fflush(stderr);
    // Skip static destructors: earlier cases may have damaged global state.
    std::_Exit(code);
  }
  if (options.m_list) {
    for (size_t i = 0; i < plan.m_cases.size(); ++i)
      std::printf("%s\n", caseId(plan, i).c_str());
    return 0;
  }
  if (!options.m_dumpId.empty()) {
    size_t index;
    if (!findCase(plan, options.m_dumpId, index) ||
        !writeFile(options.m_dumpFile, caseData(plan, index))) {
      std::fprintf(stderr, "%s: cannot dump case '%s'\n", harness.m_name,
                   options.m_dumpId.c_str());
      return 2;
    }
    return 0;
  }
  if (!options.m_seedsDir.isEmpty()) {
    QDir().mkpath(options.m_seedsDir);
    for (const Seed &seed : plan.m_seeds) {
      QString path = options.m_seedsDir + "/" +
                     QString::fromStdString(seed.m_name) + "." +
                     harness.m_extension;
      if (!writeFile(path, seed.m_data)) {
        std::fprintf(stderr, "%s: cannot write %s\n", harness.m_name,
                     path.toLocal8Bit().constData());
        return 2;
      }
    }
    return 0;
  }
  if (!options.m_caseId.empty())
    return runOneCase(harness, plan, options.m_caseId, inputPath);
  if (options.m_inProcess)
    return runInProcess(harness, options, plan, inputPath);
  return supervise(harness, options, plan, workDir + "/child");
}

//-----------------------------------------------------------------------------

int fuzzOne(const Harness &harness, const uint8_t *data, size_t size) {
  // Lives for the whole fuzzing run; libFuzzer never returns normally.
  static QTemporaryDir *directory = new QTemporaryDir();
  static const bool includeKnown =
      qEnvironmentVariableIsSet("OTFUZZ_INCLUDE_KNOWN");
  Bytes bytes(data, data + size);
  if (!includeKnown && harness.m_knownIssue && harness.m_knownIssue(bytes))
    return 0;
  const QString path =
      directory->path() + "/fuzz." + QString::fromLatin1(harness.m_extension);
  if (!writeFile(path, bytes)) return 0;
  try {
    harness.m_load(path);
  } catch (...) {
  }
  return 0;
}

}  // namespace otfuzz

//-----------------------------------------------------------------------------

#ifdef OT_LIBFUZZER
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv);

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv) {
  // Some loaders ask QCoreApplication for paths.
  static QCoreApplication *application = new QCoreApplication(*argc, *argv);
  (void)application;
  return 0;
}
#endif
