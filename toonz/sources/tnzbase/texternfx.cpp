

#include "texternfx.h"
#include "tsystem.h"
#include "tdoubleparam.h"
#include "timage_io.h"
//#include "tfx.h"
#include "trasterfx.h"
//#include "tsystem.h"
//#include "tparamcontainer.h"
#include "tfxparam.h"
#include "tstream.h"

#include <vector>
#include <cerrno>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace {

TFilePath getExternFxPath() {
  return TSystem::getBinDir() + "plugins" + "externFxs";
}

#ifdef _WIN32
DWORD expandExternalArguments(const std::string& arguments,
                              std::vector<char>& command) {
  const DWORD required =
      ExpandEnvironmentStringsA(arguments.c_str(), nullptr, 0);
  if (required == 0) return GetLastError();
  if (required == MAXDWORD) return ERROR_INSUFFICIENT_BUFFER;
  const DWORD capacity = required + 1;
  command.resize(capacity);
  const DWORD written =
      ExpandEnvironmentStringsA(arguments.c_str(), command.data(), capacity);
  if (written == 0) return GetLastError();
  return written > capacity ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS;
}
#endif

std::string runExternalProgram(const std::string& executable,
                               const std::string& arguments) {
#ifdef _WIN32
  const auto failure = [&](const char* operation, DWORD code) {
    return std::string("Cannot ") + operation + " external effect program \"" +
           executable + "\" (Windows error " + std::to_string(code) +
           "). Check the executable path, arguments and environment variables.";
  };
  std::vector<char> command;
  const DWORD expansionError = expandExternalArguments(arguments, command);
  if (expansionError != ERROR_SUCCESS)
    return failure("expand arguments for", expansionError);

  STARTUPINFOA startup = {};
  GetStartupInfoA(&startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessA(executable.c_str(), command.data(), nullptr, nullptr,
                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process))
    return failure("start", GetLastError());

  std::string error;
  DWORD exitCode = 0;
  if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_FAILED)
    error = failure("wait for", GetLastError());
  else if (!GetExitCodeProcess(process.hProcess, &exitCode))
    error = failure("read the exit status of", GetLastError());
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (!error.empty()) return error;
#else
  const std::string command = executable + arguments;
  const int status          = system(command.c_str());
  if (status == -1)
    return "Cannot start external effect program \"" + executable +
           "\": " + std::strerror(errno) +
           ". Check the executable path and arguments.";
  if (!WIFEXITED(status))
    return "External effect program \"" + executable +
           "\" terminated abnormally. Check the executable and its input "
           "files.";
  const int exitCode = WEXITSTATUS(status);
#endif
  if (exitCode != 0)
    return "External effect program \"" + executable + "\" exited with code " +
           std::to_string(exitCode) + ". Check the arguments and input files.";
  return std::string();
}

std::string readExternalOutput(const TFilePath& path, const TRaster32P& destination,
                               const std::string& executable) {
  const std::string context = "External effect program \"" + executable + "\" ";
  try {
    TRasterP decoded;
    if (!TImageReader::load(path, decoded))
      return context +
             "did not produce a readable output image. "
             "Check its output port and command arguments.";
    TRaster32P output = decoded;
    if (!output)
      return context +
             "did not produce a 32-bit RGBA output image. "
             "Check its output format.";
    if (output->getSize() != destination->getSize())
      return context +
             "produced an output image with different dimensions. "
             "Keep the output dimensions equal to the input tile.";
    destination->copy(output);
  } catch (...) {
    return "Cannot read the output image from external effect program \"" +
           executable + "\". Check its output format and arguments.";
  }
  return std::string();
}
}  // namespace

//=========================================================

// not implemented yet
void TExternFx::getNames(std::vector<std::string> &names) {}

//=========================================================

TExternFx *TExternFx::create(std::string name) {
  TExternalProgramFx *fx = new TExternalProgramFx(name);
  return fx;
}

//=========================================================

TExternalProgramFx::TExternalProgramFx(std::string name)
    : m_externFxName(name) {
  initialize(name);
  setName(L"ExternalProgramFx");
  //    addInputPort("input2", m_input2);
}

//------------------------------------------------------------------

TExternalProgramFx::TExternalProgramFx() : m_externFxName() {
  setName(L"ExternalProgramFx");
  //    addInputPort("input2", m_input2);
}

//------------------------------------------------------------------

TExternalProgramFx::~TExternalProgramFx() {}

//------------------------------------------------------------------

void TExternalProgramFx::initialize(std::string name) {
  TFilePath fp = getExternFxPath() + (name + ".xml");
  TIStream is(fp);
  if (!is) return;

  std::string tagName;
  if (!is.matchTag(tagName) || tagName != "externFx") return;

  try {
    while (is.matchTag(tagName)) {
      if (tagName == "executable") {
        TFilePath executable = TFilePath(is.getTagAttribute("path"));
        std::string args     = is.getTagAttribute("args");
        if (executable == TFilePath())
          throw TException("missing executable path");
        if (args == "") throw TException("missing args string");
        setExecutable(executable, args);
      } else if (tagName == "inport" || tagName == "outport") {
        std::string portName = is.getTagAttribute("name");
        std::string ext      = is.getTagAttribute("ext");
        if (portName == "") throw TException("missing port name");
        if (ext == "") throw TException("missing port ext");
        addPort(portName, ext, tagName == "inport");
      } else if (tagName == "param") {
        std::string paramName = is.getTagAttribute("name");
        if (paramName == "") throw TException("missing param name");
        std::string type = is.getTagAttribute("type");
        if (type == "") throw TException("missing param type");
        if (type != "double")
          throw TException("param type not yet implemented");
        TDoubleParamP param = new TDoubleParam();
        param->setName(paramName);
        m_params.push_back(param);
      } else
        throw TException("unexpected tag " + tagName);
    }
    is.closeChild();

    for (int i = 0; i < (int)m_params.size(); i++)
      bindParam(this, m_params[i]->getName(), m_params[i]);

  } catch (...) {
  }
}

//------------------------------------------------------------------

void TExternalProgramFx::addPort(std::string portName, std::string ext,
                                 bool isInput) {
  if (isInput) {
    std::unique_ptr<TRasterFxPort> port(new TRasterFxPort());
    TRasterFxPort* input = port.get();
    m_ownedPorts.push_back(std::move(port));
    m_ports[portName] = Port(portName, ext, input);
    addInputPort(portName, *input);
  } else
    m_ports[portName] = Port(portName, ext, 0);
}

//------------------------------------------------------------------

// void TExternalProgramFx::addParam(string paramName, const TParamP &param)
//{
//  m_params[paramName] = param;
//  TFx::addParam(paramName, param);
//}

//------------------------------------------------------------------

TFx *TExternalProgramFx::clone(bool recursive) const {
  TExternalProgramFx *fx =
      dynamic_cast<TExternalProgramFx *>(TExternFx::create(m_externFxName));
  assert(fx);
  // new TExternalProgramFx();
  // fx->setExecutable(m_executablePath, m_args);

  // copia della time region
  fx->setActiveTimeRegion(getActiveTimeRegion());
  // fx->m_imp->m_activeTimeRegion = m_imp->m_activeTimeRegion;

  fx->getParams()->copy(getParams());

  assert(getInputPortCount() == fx->getInputPortCount());

  // std::map<std::string, Port>::const_iterator j;
  // for(j=m_ports.begin(); j!=m_ports.end(); ++j)
  //  fx->addPort(j->first, j->second.m_ext, j->second.m_port != 0);

  // copia ricorsiva sulle porte
  if (recursive) {
    for (int i = 0; i < getInputPortCount(); ++i) {
      TFxPort *port = getInputPort(i);
      if (port->getFx())
        fx->connect(getInputPortName(i), port->getFx()->clone(true));
    }
  }

  // std::map<std::string, TParamP>::const_iterator j;
  // for(j=m_params.begin(); j!=m_params.end(); ++j)
  //  fx->addParam(j->first, j->second->clone());

  return fx;
}

//------------------------------------------------------------------

bool TExternalProgramFx::doGetBBox(double frame, TRectD &bBox,
                                   const TRenderSettings &info) {
  // bBox = TRectD(-30,-30,30,30);
  //  return true;

  std::map<std::string, Port>::const_iterator portIt;
  for (portIt = m_ports.begin(); portIt != m_ports.end(); ++portIt) {
    if (portIt->second.m_port != 0) {
      TRasterFxPort *tmp;
      tmp = portIt->second.m_port;
      if (tmp->isConnected()) {
        TRectD tmpbBox;
        (*tmp)->doGetBBox(frame, tmpbBox, info);
        bBox += tmpbBox;
      }
    }
  }

  if (bBox.isEmpty()) {
    bBox = TRectD();
    return false;
  } else
    return true;
  /*
if(m_input1.isConnected() || m_input2.isConnected())
{
bool ret = m_input1->doGetBBox(frame, bBox) || m_input1->doGetBBox(frame, bBox);
return ret;
}
else
{
bBox = TRectD();
return false;
}
*/
}

//------------------------------------------------------------------

void TExternalProgramFx::doCompute(TTile &tile, double frame,
                                   const TRenderSettings &ri) {
  TRaster32P ras = tile.getRaster();
  if (!ras) return;
  std::string args           = m_args;
  std::string executablePath = ::to_string(m_executablePath);
  std::map<std::string, TFilePath> tmpFiles;  // portname --> file
  TFilePath outputTmpFile;

  std::map<std::string, Port>::const_iterator portIt;
  for (portIt = m_ports.begin(); portIt != m_ports.end(); ++portIt) {
    TFilePath fp            = TSystem::getUniqueFile("externfx");
    fp                      = fp.withType(portIt->second.m_ext);
    tmpFiles[portIt->first] = fp;
    if (portIt->second.m_port == 0)  // solo una porta e' di output
      outputTmpFile = fp;
    else {
      TRasterFxPort *tmp;
      tmp = portIt->second.m_port;
      if (tmp->isConnected()) {
        (*tmp)->compute(tile, frame, ri);
        TImageWriter::save(fp, ras);
      }
    }
  }

  // args e' della forma "$src $ctrl -o $out -v $value"
  // sostituisco le variabili
  int i = 0;
  for (;;) {
    i = args.find('$', i);
    if (i == (int)std::string::npos) break;
    int j   = i + 1;
    int len = args.length();
    while (j < len && isalnum(args[j])) j++;
    // un '$' non seguito da caratteri alfanumerici va ignorato
    if (j == i + 1) {
      // la sequenza '$$' diventa '$'
      if (j < len && args[j] == '$') args.replace(i, 2, "$");
      i++;
      continue;
    }
    // ho trovato una variabile
    int m            = j - i - 1;
    std::string name = args.substr(i + 1, m);
    // calcolo il valore.
    std::string value;

    std::map<std::string, TFilePath>::const_iterator it;
    it = tmpFiles.find(name);
    if (it != tmpFiles.end()) {
      // e' una porta. il valore e' il nome del
      // file temporaneo
      value = "\"" + ::to_string(it->second.getWideString()) + "\"";
    } else {
      // e' un parametro
      // se il nome non viene riconosciuto sostituisco la stringa nulla
      TDoubleParamP param = TParamP(getParams()->getParam(name));
      if (param) value    = std::to_string(param->getValue(frame));
    }

    args.replace(i, m + 1, value);
  }
  args                     = " " + args;
  std::string processError = runExternalProgram(executablePath, args);

  if (processError.empty())
    processError = readExternalOutput(outputTmpFile, ras, executablePath);

  // butto i file temporanei creati
  std::map<std::string, TFilePath>::const_iterator fileIt;
  for (fileIt = tmpFiles.begin(); fileIt != tmpFiles.end(); ++fileIt) {
    if (TFileStatus(fileIt->second).doesExist() == true) try {
        TSystem::deleteFile(fileIt->second);
      } catch (...) {
      }
  }
  if (TFileStatus(outputTmpFile).doesExist() == true) try {
      TSystem::deleteFile(outputTmpFile);
    } catch (...) {
    }
  if (!processError.empty()) throw TException(processError);
}

//------------------------------------------------------------------

void TExternalProgramFx::loadData(TIStream &is) {
  std::string tagName;
  while (is.openChild(tagName)) {
    if (tagName == "path") {
      is >> m_executablePath;
    } else if (tagName == "args") {
      is >> m_args;
    } else if (tagName == "name") {
      is >> m_externFxName;
      // initialize(m_externFxName);
    } else if (tagName == "params") {
      while (is.matchTag(tagName)) {
        if (tagName == "param") {
          // assert(0);
          std::string paramName = is.getTagAttribute("name");
          TDoubleParamP param   = new TDoubleParam();
          param->setName(paramName);
          m_params.push_back(param);
        } else
          throw TException("unexpected tag " + tagName);
      }
      for (int i = 0; i < (int)m_params.size(); i++)
        bindParam(this, m_params[i]->getName(), m_params[i]);
    } else if (tagName == "ports") {
      while (is.matchTag(tagName)) {
        if (tagName == "port") {
          std::string name = is.getTagAttribute("name");
          std::string ext  = is.getTagAttribute("ext");
          addPort(name, ext, true);
        } else if (tagName == "outport") {
          std::string name = is.getTagAttribute("name");
          std::string ext  = is.getTagAttribute("ext");
          addPort(name, ext, false);
        } else
          throw TException("unexpected tag " + tagName);
      }
    } else if (tagName == "super") {
      TExternFx::loadData(is);
    } else
      throw TException("unexpected tag " + tagName);
    is.closeChild();
  }
}

void TExternalProgramFx::saveData(TOStream &os) {
  os.child("name") << m_externFxName;
  os.child("path") << m_executablePath;
  os.child("args") << m_args;
  os.openChild("params");
  for (int i = 0; i < getParams()->getParamCount(); i++) {
    std::map<std::string, std::string> attr;
    attr["name"] = getParams()->getParamName(i);
    attr["type"] = "double";
    os.openCloseChild("param", attr);
  }
  os.closeChild();
  os.openChild("ports");
  std::map<std::string, Port>::iterator portIt;
  for (portIt = m_ports.begin(); portIt != m_ports.end(); ++portIt) {
    std::map<std::string, std::string> attr;
    attr["name"]        = portIt->second.m_name;
    attr["ext"]         = portIt->second.m_ext;
    std::string tagName = portIt->second.m_port == 0 ? "outport" : "port";
    os.openCloseChild(tagName, attr);
  }
  os.closeChild();
  os.openChild("super");
  TExternFx::saveData(os);
  os.closeChild();
}

//------------------------------------------------------------------

#ifdef CICCIO
//-------------------------------------------------------------------

void ExternalProgramFx::doCompute(TTile &tile, double frame,
                                  const TRenderSettings &ri) {
  if (!m_input1.isConnected() || !m_input2.isConnected()) {
    tile.getRaster()->clear();
    return;
  }
  std::string name1("C:\\temp\\uno..tif");
  std::string name2("C:\\temp\\due..tif");
  std::string outname("C:\\temp\\outfile.0001.jpg");
  std::string program("C:\\temp\\xdissolve.exe");
  std::string extension(".jpg");
  TFilePath programpath(program);

  m_input1->compute(tile, frame, ri);
  TFilePath fname1(name1);
  TFilePath fname2(name2);
  std::string tmp1 = fname1.getName() + extension;
  std::string tmp2 = fname2.getName() + extension;

  TFilePath tmpname1(fname1.getParentDir() + tmp1);
  TFilePath tmpname2(fname2.getParentDir() + tmp2);
  TFilePath out(outname);
  TImageWriter::save(tmpname1, tile.getRaster());
  m_input2->compute(tile, frame, ri);
  TImageWriter::save(tmpname2, tile.getRaster());

  std::string arglist = " -range1 1 1 -start2 1 -startout 1 ";
  arglist += toString(tmpname1.getWideString());
  arglist += " " + toString(tmpname2.getWideString());
  arglist += " " + outname;
  std::string cmdline = program + arglist;

#ifdef _WIN32
  STARTUPINFO si;
  PROCESS_INFORMATION pinfo;

  GetStartupInfo(&si);

  BOOL ret =
      CreateProcess(NULL,                     // name of executable module
                    (char *)cmdline.c_str(),  // command line string
                    NULL,                     // SD
                    NULL,                     // SD
                    TRUE,                     // handle inheritance option
                    CREATE_NO_WINDOW, /*CREATE_NEW_CONSOLE*/  // creation flags
                    NULL,   // new environment block
                    NULL,   // current directory name
                    &si,    // startup information
                    &pinfo  // process information
                    );

  if (!ret) DWORD err = GetLastError();

  // aspetta che il processo termini
  WaitForSingleObject(pinfo.hProcess, INFINITE);

  DWORD exitCode;
  ret = GetExitCodeProcess(pinfo.hProcess,  // handle to the process
                           &exitCode);      // termination status

#else
  int exitCode = system(cmdline.c_str());
#endif
  TImageReader::load(out, tile.getRaster());

  TSystem::deleteFile(tmpname1);
  TSystem::deleteFile(tmpname2);
  TSystem::deleteFile(out);
}

FX_PLUGIN_IDENTIFIER(ExternalProgramFx, "externalProgramFx");
* /

#endif
    FX_IDENTIFIER(TExternalProgramFx, "externalProgramFx")
