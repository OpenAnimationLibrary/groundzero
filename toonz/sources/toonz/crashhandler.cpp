#include "crashhandler.h"

#include <inttypes.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <winbase.h>
#include <dbghelp.h>
#include <psapi.h>
#else
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <err.h>
#include <regex>
#endif

#include "tgl.h"
#include "tapp.h"
#include "tenv.h"
#include "tconvert.h"
#include "texception.h"
#include "tfilepath_io.h"
#include "toonz/toonzfolders.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/toonzscene.h"

#include <QOperatingSystemVersion>
#include <QDesktopServices>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QMainWindow>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDialog>
#include <QLayout>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QLocale>
#include <QStringList>
#include <QtGlobal>

static QWidget *s_parentWindow = NULL;
static bool s_reportProjInfo   = false;
static QString s_previousReport;
static TFilePath s_previousReportFile;
#ifdef ENABLE_CRASH_REPORTER_TEST
static volatile sig_atomic_t s_intentionalCrashTest = 0;
#endif

static void appendUtf8(std::string &out, const QString &text) {
  const QByteArray utf8 = text.toUtf8();
  out.append(utf8.constData(), utf8.size());
}

static void appendPath(std::string &out, const char *label,
                       const QString &path) {
  out.append(label);
  appendUtf8(out, path);
  for (const QChar c : path) {
    if (c.unicode() > 0x7f) {
      out.append(" [contains non-ASCII characters]");
      break;
    }
  }
  out.append("\n");
}

//-----------------------------------------------------------------------------

static const char *filenameOnly(const char *path) {
  for (int i = strlen(path); i >= 0; --i) {
    if (path[i] == '\\' || path[i] == '/') return path + i + 1;
  }
  return path;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Windows platform functions

#ifdef _WIN32

static PEXCEPTION_POINTERS s_exceptionInfo = NULL;

#define HAS_MINIDUMP
static bool generateMinidump(TFilePath dumpFile) {
  HANDLE hDumpFile = CreateFileW(dumpFile.getWideString().c_str(),
                                 GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
  if (hDumpFile == INVALID_HANDLE_VALUE) return false;

  MINIDUMP_EXCEPTION_INFORMATION mdei;
  mdei.ThreadId          = GetCurrentThreadId();
  mdei.ExceptionPointers = s_exceptionInfo;
  mdei.ClientPointers    = FALSE;

  if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile,
                        MiniDumpNormal, s_exceptionInfo ? &mdei : NULL, 0,
                        NULL)) {
    CloseHandle(hDumpFile);
    return true;
  }

  CloseHandle(hDumpFile);
  return false;
}

#define HAS_MODULES
static void printModules(std::string &out) {
  HANDLE hProcess = GetCurrentProcess();

  HMODULE modules[1024];
  DWORD size;
  if (EnumProcessModules(hProcess, modules, sizeof(modules), &size)) {
    const unsigned int moduleCapacity =
        static_cast<unsigned int>(sizeof(modules) / sizeof(HMODULE));
    unsigned int moduleCount =
        static_cast<unsigned int>(size / sizeof(HMODULE));
    if (moduleCount > moduleCapacity) moduleCount = moduleCapacity;
    for (unsigned int i = 0; i < moduleCount; i++) {
      wchar_t moduleName[512] = {};
      GetModuleFileNameW(modules[i], moduleName, 512);
      appendUtf8(out, QString::fromWCharArray(moduleName));
      out.append("\n");
    }
  }
}

static void printTabletInfo(std::string &out) {
  QStringList modules;
  if (GetModuleHandleW(L"wintab32.dll")) modules.append("wintab32.dll");
  if (GetModuleHandleW(L"Wacom_Tablet.dll")) modules.append("Wacom_Tablet.dll");

  out.append("Tablet Modules: ");
  appendUtf8(out,
             modules.isEmpty() ? QString("None detected") : modules.join(", "));
  out.append("\n");
}

static void printWindowsVersion(std::string &out) {
  typedef LONG(WINAPI * RtlGetVersionPtr)(OSVERSIONINFOW *);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  RtlGetVersionPtr rtlGetVersion =
      ntdll ? reinterpret_cast<RtlGetVersionPtr>(
                  GetProcAddress(ntdll, "RtlGetVersion"))
            : NULL;

  OSVERSIONINFOW version      = {};
  version.dwOSVersionInfoSize = sizeof(version);
  if (!rtlGetVersion || rtlGetVersion(&version) != 0) {
    out.append("Operating System: Windows (native version unavailable)\n");
    return;
  }

  out.append("Operating System: ");
  if (version.dwMajorVersion == 10)
    out.append(version.dwBuildNumber >= 22000 ? "Windows 11\n"
                                              : "Windows 10\n");
  else
    out.append("Windows " + std::to_string(version.dwMajorVersion) + "." +
               std::to_string(version.dwMinorVersion) + "\n");
  out.append("OS Kernel: " + std::to_string(version.dwMajorVersion) + "." +
             std::to_string(version.dwMinorVersion) + "." +
             std::to_string(version.dwBuildNumber) + " (native)\n");
  out.append("Qt OS Description: ");
  appendUtf8(out, QSysInfo::prettyProductName());
  out.append("\n");
}

#define HAS_BACKTRACE
static void printBacktrace(std::string &out) {
  int frameStack = 0;
  int frameSkip  = 3;

  HANDLE hProcess = GetCurrentProcess();

  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_INCLUDE_32BIT_MODULES |
                SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES |
                SYMOPT_UNDNAME);

  SymInitialize(hProcess, NULL, TRUE);

  CONTEXT context;
  RtlCaptureContext(&context);

  char sourceSymMem[sizeof(IMAGEHLP_SYMBOL64) + 1025];
  PIMAGEHLP_SYMBOL64 sourceSym = (PIMAGEHLP_SYMBOL64)&sourceSymMem;
  memset(sourceSymMem, 0, sizeof(sourceSymMem));

  IMAGEHLP_LINE64 sourceInfo;
  memset(&sourceInfo, 0, sizeof(IMAGEHLP_LINE64));
  sourceInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

  IMAGEHLP_MODULE64 moduleInfo;
  memset(&moduleInfo, 0, sizeof(moduleInfo));
  moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

  STACKFRAME64 stackframe;
  memset(&stackframe, 0, sizeof(STACKFRAME64));

#ifdef _WIN64
  int machineType             = IMAGE_FILE_MACHINE_AMD64;
  stackframe.AddrPC.Offset    = context.Rip;
  stackframe.AddrPC.Mode      = AddrModeFlat;
  stackframe.AddrStack.Offset = context.Rsp;
  stackframe.AddrStack.Mode   = AddrModeFlat;
  stackframe.AddrFrame.Offset = context.Rbp;
  stackframe.AddrFrame.Mode   = AddrModeFlat;
#else
  int machineType             = IMAGE_FILE_MACHINE_I386;
  stackframe.AddrPC.Offset    = context.Eip;
  stackframe.AddrPC.Mode      = AddrModeFlat;
  stackframe.AddrStack.Offset = context.Esp;
  stackframe.AddrStack.Mode   = AddrModeFlat;
  stackframe.AddrFrame.Offset = context.Ebp;
  stackframe.AddrFrame.Mode   = AddrModeFlat;
#endif

  HANDLE hThread = GetCurrentThread();
  while (StackWalk64(machineType, hProcess, hThread, &stackframe, &context,
                     NULL, SymFunctionTableAccess64, SymGetModuleBase64,
                     NULL)) {
    // Skip first frames since they point to this function
    if (frameStack++ < frameSkip) continue;
    char numStr[32];
    memset(numStr, 0, sizeof(numStr));
    sprintf(numStr, "%3i> ", frameStack - frameSkip);
    out.append(numStr);

    sourceSym->SizeOfStruct  = sizeof(IMAGEHLP_SYMBOL64);
    sourceSym->MaxNameLength = 1024;

    // Get symbol name
    DWORD64 displacement64;
    if (SymGetSymFromAddr64(hProcess, (ULONG64)stackframe.AddrPC.Offset,
                            &displacement64, sourceSym)) {
      out.append(sourceSym->Name);

      // Get module filename
      char moduleFile[512];
      MEMORY_BASIC_INFORMATION mbi;
      VirtualQuery((LPCVOID)stackframe.AddrPC.Offset, &mbi, sizeof(mbi));
      GetModuleFileNameA((HMODULE)mbi.AllocationBase, moduleFile, 512);

      // Get source filename and line
      DWORD displacement32;
      if (SymGetLineFromAddr64(hProcess, stackframe.AddrPC.Offset,
                               &displacement32, &sourceInfo) != FALSE) {
        out.append(" {");
        out.append(filenameOnly(sourceInfo.FileName));
        out.append(":");
        out.append(std::to_string(sourceInfo.LineNumber));
        out.append("}");
      } else {
        memset(numStr, 0, sizeof(numStr));
        sprintf(numStr, " [0x%" PRIx64 "]", stackframe.AddrPC.Offset);
        out.append(numStr);
      }
      out.append(" <");
      out.append(filenameOnly(moduleFile));
      out.append(">");
    }

    out.append("\n");
  }

  SymCleanup(hProcess);
}

//-----------------------------------------------------------------------------

LONG WINAPI exceptionHandler(PEXCEPTION_POINTERS info) {
  static volatile bool handling = false;

  const char *reason = "Unknown";
  switch (info->ExceptionRecord->ExceptionCode) {
  case EXCEPTION_ACCESS_VIOLATION:
    reason = "EXCEPTION_ACCESS_VIOLATION";
    break;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    reason = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    break;
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    reason = "EXCEPTION_DATATYPE_MISALIGNMENT";
    break;
  case EXCEPTION_FLT_DENORMAL_OPERAND:
    reason = "EXCEPTION_FLT_DENORMAL_OPERAND";
    break;
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    reason = "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    break;
  case EXCEPTION_FLT_INEXACT_RESULT:
    reason = "EXCEPTION_FLT_INEXACT_RESULT";
    break;
  case EXCEPTION_FLT_INVALID_OPERATION:
    reason = "EXCEPTION_FLT_INVALID_OPERATION";
    break;
  case EXCEPTION_FLT_OVERFLOW:
    reason = "EXCEPTION_FLT_OVERFLOW";
    break;
  case EXCEPTION_FLT_STACK_CHECK:
    reason = "EXCEPTION_FLT_STACK_CHECK";
    break;
  case EXCEPTION_FLT_UNDERFLOW:
    reason = "EXCEPTION_FLT_UNDERFLOW";
    break;
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    reason = "EXCEPTION_ILLEGAL_INSTRUCTION";
    break;
  case EXCEPTION_IN_PAGE_ERROR:
    reason = "EXCEPTION_IN_PAGE_ERROR";
    break;
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    reason = "EXCEPTION_INT_DIVIDE_BY_ZERO";
    break;
  case EXCEPTION_INVALID_DISPOSITION:
    reason = "EXCEPTION_INVALID_DISPOSITION";
    break;
  case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    reason = "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    break;
  case EXCEPTION_PRIV_INSTRUCTION:
    reason = "EXCEPTION_PRIV_INSTRUCTION";
    break;
  case EXCEPTION_STACK_OVERFLOW:
    reason = "EXCEPTION_STACK_OVERFLOW";
    break;
  default:
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Avoid new exceptions inside the crash handler
  if (handling) return EXCEPTION_CONTINUE_SEARCH;

  handling = true;
  s_exceptionInfo = info;
  if (CrashHandler::trigger(reason, true)) _Exit(1);
  s_exceptionInfo = NULL;
  handling = false;

  return EXCEPTION_CONTINUE_SEARCH;
}

#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Linux and Mac OS X platform functions

#ifndef _WIN32

static bool sh(std::string &out, const char *cmd) {
  char buffer[128];
  FILE *p = popen(cmd, "r");
  if (p == NULL) return false;
  while (fgets(buffer, 128, p)) out.append(buffer);
  pclose(p);
  return true;
}

//-----------------------------------------------------------------------------

static bool addr2line(std::string &out, const char *exepath, const char *addr) {
  char cmd[512];
#ifdef MACOSX
  sprintf(cmd, "atos -o \"%.400s\" %s 2>&1", exepath, addr);
#else
  sprintf(cmd, "addr2line -f -p -e \"%.400s\" %s 2>&1", exepath, addr);
#endif
  return sh(out, cmd);
}

//-----------------------------------------------------------------------------

static bool generateMinidump(TFilePath dumpFile) { return false; }

//-----------------------------------------------------------------------------

static void printModules(std::string &out) {}

static void printTabletInfo(std::string &out) {}

//-----------------------------------------------------------------------------

#define HAS_BACKTRACE
static void printBacktrace(std::string &out) {
  int frameStack = 0;
  int frameSkip  = 3;

  const int size = 256;
  void *buffer[size];

  // Get executable path
  char exepath[512];
  memset(exepath, 0, 512);
  if (readlink("/proc/self/exe", exepath, 512) < 0)
    fprintf(stderr, "Couldn't get exe path\n");

  // Back trace
  int nptrs  = backtrace(buffer, size);
  char **bts = backtrace_symbols(buffer, nptrs);
  std::regex re("\\[(.+)\\]");
  if (bts) {
    for (int i = 0; i < nptrs; ++i) {
      // Skip first frames since they point to this function
      if (frameStack++ < frameSkip) continue;
      char numStr[32];
      memset(numStr, 0, sizeof(numStr));
      sprintf(numStr, "%3i> ", frameStack - frameSkip);
      out.append(numStr);

      std::string sym = bts[i];
      std::string line;
      std::smatch ms;

      bool found = false;
      if (std::regex_search(sym, ms, re)) {
        std::string addr = ms[1];
        if (addr2line(line, exepath, addr.c_str())) {
          found = (line.rfind("??", 0) != 0);
        }
      }

      out.append(found ? line : (sym + "\n"));
    }
  }

  free(bts);
}

//-----------------------------------------------------------------------------
// Async-signal-safe crash report
//
// After a crash the heap, stdio and Qt may be corrupted, so the signal handler
// only writes data prepared by CrashHandler::prepareReport() plus a raw
// backtrace into a pending report file. The report is shown on next launch by
// CrashHandler::showPendingReport().

static char s_pendingReportPath[4096] = {0};
static char s_reportIntro[1024]       = {0};
static size_t s_reportIntroLen        = 0;
static char s_reportSysInfo[8192]     = {0};
static size_t s_reportSysInfoLen      = 0;

static size_t copyToBuffer(char *dst, size_t capacity, const char *src,
                           size_t len) {
  if (len > capacity - 1) len = capacity - 1;
  memcpy(dst, src, len);
  dst[len] = 0;
  return len;
}

static void writeAll(int fd, const char *data, size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, data, len);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return;
    }
    data += n;
    len -= (size_t)n;
  }
}

static void writeText(int fd, const char *text) {
  writeAll(fd, text, strlen(text));
}

static void writeNumber(int fd, unsigned long long value, int minDigits) {
  char buf[24];
  int pos = sizeof(buf);
  do {
    buf[--pos] = (char)('0' + value % 10);
    value /= 10;
    --minDigits;
  } while (pos > 0 && (value > 0 || minDigits > 0));
  writeAll(fd, buf + pos, sizeof(buf) - pos);
}

// localtime() and strftime() are not async-signal-safe, so convert to a UTC
// date with plain arithmetic (days-to-civil algorithm by Howard Hinnant).
static void writeUtcTime(int fd, time_t t) {
  long long days = (long long)t / 86400;
  long long secs = (long long)t % 86400;
  if (secs < 0) {
    secs += 86400;
    --days;
  }
  days += 719468;
  const long long era  = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe   = (unsigned)(days - era * 146097);
  const unsigned yoe   = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy   = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp    = (5 * doy + 2) / 153;
  const unsigned day   = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  const long long year = (long long)yoe + era * 400 + (month <= 2 ? 1 : 0);

  writeNumber(fd, (unsigned long long)year, 4);
  writeText(fd, "-");
  writeNumber(fd, month, 2);
  writeText(fd, "-");
  writeNumber(fd, day, 2);
  writeText(fd, " ");
  writeNumber(fd, (unsigned long long)(secs / 3600), 2);
  writeText(fd, ":");
  writeNumber(fd, (unsigned long long)(secs / 60 % 60), 2);
  writeText(fd, ":");
  writeNumber(fd, (unsigned long long)(secs % 60), 2);
  writeText(fd, " UTC");
}

void signalHandler(int sig) {
  static volatile sig_atomic_t handling = 0;

  const char *reason = "Unknown";
  switch (sig) {
  case SIGABRT:
    reason = "(SIGABRT) Usually caused by an abort() or assert()";
    break;
  case SIGFPE:
    reason = "(SIGFPE) Arithmetic exception, such as divide by zero";
    break;
  case SIGILL:
    reason = "(SIGILL) Illegal instruction";
    break;
  case SIGSEGV:
    reason = "(SIGSEGV) Segmentation Fault";
    break;
#ifdef SIGBUS
  case SIGBUS:
    reason = "(SIGBUS) Bus error, such as an invalid memory access";
    break;
#endif
  }

  // Only async-signal-safe calls below. Avoid new signals inside the crash
  // handler: a second crash goes straight to the default action.
  if (!handling && s_pendingReportPath[0]) {
    handling = 1;
    int fd   = open(s_pendingReportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      writeAll(fd, s_reportIntro, s_reportIntroLen);
      writeText(fd, "\nReport Date: ");
      writeUtcTime(fd, time(NULL));
      writeText(fd, "\nCrash Reason: ");
      writeText(fd, reason);
#ifdef ENABLE_CRASH_REPORTER_TEST
      if (s_intentionalCrashTest)
        writeText(fd, "\nCrash Test: Intentional (Help > Test Crash Reporter)");
#endif
      writeText(fd, "\n\n");
      writeAll(fd, s_reportSysInfo, s_reportSysInfoLen);

      // Raw frames; the first one is this handler
      writeText(fd, "\n==== Backtrace ====\n");
      void *frames[256];
      int nptrs = backtrace(frames, 256);
      if (nptrs > 1) backtrace_symbols_fd(frames + 1, nptrs - 1, fd);
      writeText(fd, "==== End ====\n");
      close(fd);
    }
  }
  handling = 1;

  // Let the OS perform its normal crash handling (core dump, system crash
  // reporter, exit status)
  signal(sig, SIG_DFL);
  raise(sig);
}

#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

static void printSysInfo(std::string &out) {
  out.append("Build ABI: ");
  appendUtf8(out, QSysInfo::buildAbi());
  out.append("\n");
#ifdef _WIN32
  printWindowsVersion(out);
#else
  out.append("Operating System: ");
  appendUtf8(out, QSysInfo::prettyProductName());
  out.append("\nOS Kernel: ");
  appendUtf8(out, QSysInfo::kernelVersion());
  out.append("\n");
#endif
  out.append("CPU Threads: " + std::to_string(QThread::idealThreadCount()) + "\n");
  out.append("Qt Build Version: " QT_VERSION_STR "\n");
  out.append("Qt Runtime Version: ");
  out.append(qVersion());
  out.append("\n");
  printTabletInfo(out);
}

//-----------------------------------------------------------------------------

static void printGPUInfo(std::string &out) {
  const char *gpuVendorName = (const char *)glGetString(GL_VENDOR);
  const char *gpuModelName  = (const char *)glGetString(GL_RENDERER);
  const char *gpuVersion    = (const char *)glGetString(GL_VERSION);
  if (gpuVendorName)
    out.append("GPU Vendor: " + std::string(gpuVendorName) + "\n");
  if (gpuModelName)
    out.append("GPU Model: " + std::string(gpuModelName) + "\n");
  if (gpuVersion)
    out.append("OpenGL / Driver: " + std::string(gpuVersion) + "\n");
}

//-----------------------------------------------------------------------------

static void printUpdateGuidance(std::string &out) {
  const QDate buildDate =
      QLocale::c().toDate(QString::fromLatin1(__DATE__), "MMM d yyyy");
  if (!buildDate.isValid()) return;

  const int buildAge = buildDate.daysTo(QDate::currentDate());
  if (buildAge < 30) return;

  out.append("\nUpdate Test Recommended: This build is " +
             std::to_string(buildAge) + " days old. ");
#if defined(_WIN32) || defined(MACOSX)
  out.append(
      "Before reporting, reproduce the problem with the latest nightly build "
      "if possible; it may already be fixed.\n"
      "Nightly Builds: "
      "https://opentoonz.github.io/e/download/opentoonz.html\n");
#else
  out.append(
      "Before reporting, reproduce the problem with a recent package or a "
      "build from current source if possible; it may already be fixed. "
      "Official nightly downloads are currently provided for Windows and "
      "macOS.\n");
#endif
}

//-----------------------------------------------------------------------------

CrashHandler::CrashHandler(QWidget *parent, TFilePath crashFile,
                           QString crashReport, bool previousSession)
    : QDialog(parent)
    , m_crashFile(crashFile)
    , m_crashReport(crashReport)
    , m_previousSession(previousSession) {
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);

  QStringList sl;
  if (previousSession)
    sl.append(tr("<b>OpenToonz crashed during the previous session.</b>"));
  else
    sl.append(tr("<b>OpenToonz crashed unexpectedly.</b>"));
  sl.append("");
  sl.append(tr("A crash report has been generated."));
  sl.append(
      tr("To report, click 'Open Issue Webpage' to access the OT-Dev Issues "
         "page on GitHub."));
  sl.append(tr("Click on the 'New issue' button and fill out the form."));
  sl.append("");
  sl.append(tr("System Configuration and Problem Details:"));
  
  QLabel *headtext = new QLabel(sl.join("<br>"));
  headtext->setTextFormat(Qt::RichText);

  QTextEdit *reportTxt = new QTextEdit();
  reportTxt->setText(crashReport);
  reportTxt->setReadOnly(true);
  reportTxt->setLineWrapMode(QTextEdit::LineWrapMode::NoWrap);
  reportTxt->setStyleSheet(
      "background:white;\ncolor:black;\nborder:1 solid black;");

  QVBoxLayout *mainLayout = new QVBoxLayout();
  QHBoxLayout *buttonsLay = new QHBoxLayout();

  QPushButton *copyBtn   = new QPushButton(tr("Copy to Clipboard"));
  QPushButton *webBtn    = new QPushButton(tr("Open Issue Webpage"));
  QPushButton *folderBtn = new QPushButton(tr("Open Reports Folder"));
  QPushButton *closeBtn =
      new QPushButton(previousSession ? tr("Close") : tr("Close Application"));
  buttonsLay->addWidget(copyBtn);
  buttonsLay->addWidget(webBtn);
  buttonsLay->addWidget(folderBtn);
  buttonsLay->addWidget(closeBtn);

  mainLayout->addWidget(headtext);
  mainLayout->addWidget(reportTxt);
  mainLayout->addLayout(buttonsLay);

  bool ret = connect(copyBtn, SIGNAL(clicked()), this, SLOT(copyClipboard()));
  ret = ret && connect(webBtn, SIGNAL(clicked()), this, SLOT(openWebpage()));
  ret = ret && connect(folderBtn, SIGNAL(clicked()), this, SLOT(openFolder()));
  ret = ret && connect(closeBtn, SIGNAL(clicked()), this, SLOT(accept()));
  if (!ret) throw TException();

  setWindowTitle(previousSession ? tr("OpenToonz Crash Report")
                                 : tr("OpenToonz crashed!"));
  setLayout(mainLayout);
}

void CrashHandler::reject() {
  // The application was restarted already, nothing to resume
  if (m_previousSession) {
    QDialog::reject();
    return;
  }

  QStringList sl;
  sl.append(tr("Application is in unstable state and must be restarted."));
  sl.append(tr("Resuming is not recommended and may lead to an unrecoverable crash."));
  sl.append(tr("Ignore advice and try to resume program?"));

  QMessageBox::StandardButton reply =
      QMessageBox::question(this, tr("Ignore crash?"), sl.join("\n"),
                            QMessageBox::Yes | QMessageBox::No);
  if (reply == QMessageBox::Yes) {
    QDialog::reject();
  }
}

//-----------------------------------------------------------------------------

void CrashHandler::copyClipboard() {
  QClipboard *clipboard = QApplication::clipboard();
  clipboard->setText(m_crashReport);
}

//-----------------------------------------------------------------------------

void CrashHandler::openWebpage() {
  // OT-Dev is unofficial: its crash reports must not go to upstream OpenToonz
  QDesktopServices::openUrl(
      QUrl("https://github.com/OpenAnimationLibrary/opentoonz-dev/issues"));
}  

//-----------------------------------------------------------------------------

void CrashHandler::openFolder() {
  TFilePath fp = ToonzFolder::getCrashReportFolder();
  QDesktopServices::openUrl(QUrl("file:///" + fp.getQString()));
}

//-----------------------------------------------------------------------------

void CrashHandler::install() {
#ifdef _WIN32
  // std library seems to override this
  //SetUnhandledExceptionFilter(exceptionHandler);

  void *handler = AddVectoredExceptionHandler(0, exceptionHandler);
  assert(handler != NULL);
  //RemoveVectoredExceptionHandler(handler);
#else
  // backtrace() may allocate while loading the unwinder on its first call,
  // which is not safe inside a signal handler, so preload it now.
  void *frames[1];
  backtrace(frames, 1);

  // SIGINT and SIGTERM are not crashes and keep their default behavior.
  signal(SIGABRT, signalHandler);
  signal(SIGFPE, signalHandler);
  signal(SIGILL, signalHandler);
  signal(SIGSEGV, signalHandler);
#ifdef SIGBUS
  signal(SIGBUS, signalHandler);
#endif
#endif
}

//-----------------------------------------------------------------------------

void CrashHandler::prepareReport() {
#ifndef _WIN32
  // Everything the signal handler writes besides the backtrace is gathered
  // here, while it is still safe to use Qt and the heap.
  TFilePath folder = ToonzFolder::getCrashReportFolder();
  if (folder.isEmpty()) return;

  std::string intro;
  std::string sysInfo;
  try {
    intro.append("Application Version: " + TEnv::getApplicationFullName() +
                 "\n");
    intro.append("Build Date: " __DATE__ "\n");
    printSysInfo(sysInfo);
    printUpdateGuidance(sysInfo);
    sysInfo.append(
        "\nReport Type: Written at crash time with async-signal-safe calls "
        "only; GPU and project details are not included\n");
  } catch (...) {
  }
  s_reportIntroLen   = copyToBuffer(s_reportIntro, sizeof(s_reportIntro),
                                    intro.c_str(), intro.size());
  s_reportSysInfoLen = copyToBuffer(s_reportSysInfo, sizeof(s_reportSysInfo),
                                    sysInfo.c_str(), sysInfo.size());

  // Take over the report of a crash in the previous session before a new
  // crash can overwrite it. It is kept with the others under its crash time
  // and shown once by showPendingReport().
  const TFilePath fpPending = folder + "PendingCrashReport.log";
  const QString pendingPath = fpPending.getQString();
  QFile file(pendingPath);
  if (file.exists()) {
    if (file.open(QIODevice::ReadOnly)) {
      s_previousReport = QString::fromUtf8(file.readAll());
      file.close();
    }
    const QDateTime crashTime = QFileInfo(pendingPath).lastModified();
    s_previousReportFile =
        folder + ("Crash-" + crashTime.toString("yyyyMMdd-HHmmss") + ".log")
                     .toStdString();
    if (!QFile::rename(pendingPath, s_previousReportFile.getQString())) {
      QFile::remove(pendingPath);
      s_previousReportFile = TFilePath();
    }
  }

  // Set the path last: the handler writes nothing while it is empty
  const QByteArray path = QFile::encodeName(pendingPath);
  if (path.size() < (int)sizeof(s_pendingReportPath))
    copyToBuffer(s_pendingReportPath, sizeof(s_pendingReportPath),
                 path.constData(), path.size());
#endif
}

//-----------------------------------------------------------------------------

void CrashHandler::showPendingReport() {
  if (s_previousReport.isEmpty()) return;

  const QString report = s_previousReport;
  s_previousReport.clear();

  CrashHandler crashdialog(s_parentWindow, s_previousReportFile, report, true);
  crashdialog.exec();
}

//-----------------------------------------------------------------------------

void CrashHandler::reportProjectInfo(bool enableReport) {
  s_reportProjInfo = enableReport;
}

//-----------------------------------------------------------------------------

void CrashHandler::attachParentWindow(QWidget *parent) {
  s_parentWindow = parent;
}

//-----------------------------------------------------------------------------

#ifdef ENABLE_CRASH_REPORTER_TEST
void CrashHandler::triggerTestCrash() {
  s_intentionalCrashTest = 1;
#ifdef _WIN32
  RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, NULL);
#else
  raise(SIGSEGV);
#endif
}
#endif

//-----------------------------------------------------------------------------

bool CrashHandler::trigger(const QString reason, bool showDialog) {
  char fileName[128];
  char dumpName[128];
  char dateName[128];
  std::string out;

  // Get time and build filename
  time_t acc_time;
  time(&acc_time);
  struct tm *tm = localtime(&acc_time);
  strftime(dateName, 128, "%Y-%m-%d %H:%M:%S", tm);
  strftime(fileName, 128, "Crash-%Y%m%d-%H%M%S.log", tm);
  strftime(dumpName, 128, "Crash-%Y%m%d-%H%M%S.dmp", tm);
  TFilePath fpCrsh = ToonzFolder::getCrashReportFolder() + fileName;
  TFilePath fpDump = ToonzFolder::getCrashReportFolder() + dumpName;

  // Generate minidump
  bool minidump = generateMinidump(fpDump);

  // Generate report
  try {
    out.append("Application Version: " + TEnv::getApplicationFullName() + "\n");
    out.append("Build Date: " __DATE__ "\n");
    out.append("\nReport Date: ");
    out.append(dateName);
    out.append("\nCrash Reason: ");
    out.append(reason.toStdString());
#ifdef ENABLE_CRASH_REPORTER_TEST
    if (s_intentionalCrashTest)
      out.append("\nCrash Test: Intentional (Help > Test Crash Reporter)");
#endif
    out.append("\n\n");
    printSysInfo(out);
    printUpdateGuidance(out);
    out.append("\n");
    printGPUInfo(out);
    out.append("\n");
    appendPath(out, "Crash File: ", fpCrsh.getQString());
#ifdef HAS_MINIDUMP
    out.append("Mini Dump File: ");
    if (minidump)
      appendPath(out, "", fpDump.getQString());
    else
      out.append("Failed\n");
#endif
  } catch (...) {
  }
  try {
    if (s_reportProjInfo) {
      auto currentProject = TProjectManager::instance()->getCurrentProject();
      TFilePath projectPath    = currentProject->getProjectPath();

      ToonzScene *currentScene = TApp::instance()->getCurrentScene()->getScene();
      std::wstring sceneName   = currentScene->getSceneName();

      out.append("\n");
      appendPath(out,
                 "Application Dir: ", QCoreApplication::applicationDirPath());
      appendPath(out, "Stuff Dir: ", TEnv::getStuffDir().getQString());
      out.append("\nProject Name: ");
      appendUtf8(out, currentProject->getName().getQString());
      out.append("\nScene Name: ");
      appendUtf8(out, QString::fromStdWString(sceneName));
      out.append("\n");
      appendPath(out, "Project Path: ", projectPath.getQString());
      appendPath(out,
                 "Scene Path: ", currentScene->getScenePath().getQString());
    }
  } catch (...) {
  }
#ifdef HAS_MODULES
  try {
    out.append("\n==== Modules ====\n");
    printModules(out);
    out.append("==== End ====\n");
  } catch (...) {
  }
#endif
#ifdef HAS_BACKTRACE
  try {
    out.append("\n==== Backtrace ====\n");
    printBacktrace(out);
    out.append("==== End ====\n");
  } catch (...) {
  }
#endif

  // Save to crash information to file
  FILE *fw = fopen(fpCrsh, "w");
  if (fw != NULL) {
    fwrite(out.c_str(), 1, out.size(), fw);
    fclose(fw);
  }

  if (showDialog) {
    // Show crash handler dialog
    CrashHandler crashdialog(s_parentWindow, fpCrsh, QString::fromStdString(out));
    return crashdialog.exec() != QDialog::Rejected;
  }

  return true;
}

//-----------------------------------------------------------------------------
