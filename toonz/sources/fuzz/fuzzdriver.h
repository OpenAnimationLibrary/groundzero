#pragma once

#ifndef OTFUZZ_FUZZDRIVER_H
#define OTFUZZ_FUZZDRIVER_H

// Shared driver for the file loader robustness tests.
//
// Every harness (tstream_fuzz.cpp, pli_fuzz.cpp, psd_fuzz.cpp) describes one
// loader:
// how to build a few small valid seed files with the existing writers, how
// to load a file the way OpenToonz does, and which inputs reach a loader bug
// that is already known. The same harness is built in two ways:
//
//  - <name>_fuzz_test (BUILD_FUZZ_TESTS): a normal executable registered
//    with ctest. It loads every seed and a fixed, deterministic set of
//    corruptions of it (truncations, bit flips, byte overwrites, corrupted
//    length fields, and text edits for the XML-like formats). The cases run
//    in child processes, so a crash or a hang is reported with its case id
//    and the remaining cases still run.
//
//  - <name>_fuzzer (WITH_LIBFUZZER, Clang only): a libFuzzer binary built
//    from LLVMFuzzerTestOneInput. It is not registered with ctest.
//
// Command line of the ctest executables:
//
//   (no arguments)       run all cases, report every failure, exit 1 if any
//   --list               print the case ids
//   --case <id>          run one case in this process (for a debugger)
//   --dump <id> <file>   write the bytes of one case to a file
//   --write-seeds <dir>  write the generated seeds (a libFuzzer corpus)
//   --corpus <dir>       also use every file in <dir> as a seed
//   --include-known      also run inputs that reach a known loader bug
//   --only-known         run only those inputs
//   --in-process         run all cases in this process, without children
//   --timeout <seconds>  per case time limit (default 20)
//
// Reproducing a failure: a case id ("<seed>@<mutation>", e.g.
// "two-frames@trunc:120") holds everything needed to rebuild its input, and
// the report prints the commands for every failing case:
//
//   pli_fuzz_test --case 'two-frames@trunc:120'        # in a debugger
//   pli_fuzz_test --dump 'two-frames@trunc:120' bad.pli
//
// The dumped file can then be opened in OpenToonz or passed to a <name>_fuzzer
// binary. A crash found by libFuzzer is replayed with "<name>_fuzzer
// <crash file>"; set OTFUZZ_INCLUDE_KNOWN=1 to also run inputs that reach a
// known issue.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class QString;

namespace otfuzz {

typedef std::vector<uint8_t> Bytes;

//! A binary field that holds a length, count, size or offset. The driver
//! overwrites these with boundary values.
struct Field {
  size_t m_offset;
  int m_bytes;  // 1, 2 or 4
  bool m_bigEndian;
};

struct Seed {
  std::string m_name;  // must not contain '@'
  Bytes m_data;
  std::vector<Field> m_fields;
};

//! A loader bug found while writing the harness. Inputs that reach it are
//! skipped by default and counted in the report, so that the test result
//! shows new problems. Remove the entry once the loader is fixed.
struct KnownIssue {
  const char *m_id;
  const char *m_summary;
};

struct Harness {
  const char *m_name;       // used in messages
  const char *m_extension;  // extension of the file given to the loader
  bool m_textual;           // add text mutations
  //! Builds the seeds. workDir is an empty scratch directory.
  std::vector<Seed> (*m_makeSeeds)(const QString &workDir);
  //! Loads the file like OpenToonz does. May throw.
  void (*m_load)(const QString &path);
  //! Returns the known bug that the input reaches, or nullptr.
  const KnownIssue *(*m_knownIssue)(const Bytes &data);
};

//! Runs the ctest driver. Needs a QCoreApplication.
int runDriver(const Harness &harness, int argc, char *argv[]);

//! Loads one input; used by LLVMFuzzerTestOneInput.
int fuzzOne(const Harness &harness, const uint8_t *data, size_t size);

//! Reads a whole file; returns false if it cannot be read.
bool readFile(const QString &path, Bytes &data);

//-----------------------------------------------------------------------------

//! Builds binary seeds and records their length fields.
class Writer {
  bool m_bigEndian;

public:
  Bytes m_data;
  std::vector<Field> m_fields;

  explicit Writer(bool bigEndian) : m_bigEndian(bigEndian) {}

  size_t pos() const { return m_data.size(); }

  void u8(unsigned value, bool isField = false);
  void u16(unsigned value, bool isField = false);
  void u32(uint32_t value, bool isField = false);
  void bytes(const void *data, size_t size);
  void text(const char *str);  // without the terminating 0
  void zeros(size_t count);
  void f64(double value);  // host byte order, as fwrite() stores it

  void patchU16(size_t offset, unsigned value);
  void patchU32(size_t offset, uint32_t value);
  void addField(size_t offset, int size);
};

//! Bounds-checked readers for the known issue checks.
bool has(const Bytes &data, size_t offset, size_t count);
unsigned getU16(const Bytes &data, size_t offset, bool bigEndian);
uint32_t getU32(const Bytes &data, size_t offset, bool bigEndian);

}  // namespace otfuzz

#endif  // OTFUZZ_FUZZDRIVER_H
