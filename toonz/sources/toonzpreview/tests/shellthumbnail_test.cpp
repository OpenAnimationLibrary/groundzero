#include "../toonzpreview.h"
#include "../thumbnailprocess.h"
#include "toonz/shellthumbnail.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/tcamera.h"
#include "toonz/sceneproperties.h"
#include "tenv.h"
#include "tiio_std.h"
#include "tnzimage.h"
#include "tthread.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "tpalette.h"
#include "tsimplecolorstyles.h"
#include "tlevel_io.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <exception>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
struct Pixels { unsigned width = 0, height = 0; std::vector<unsigned char> bytes; };
using FactoryEntry = HRESULT (STDAPICALLTYPE *)(REFCLSID, REFIID, void **);
Pixels payload(const QString &path) {
  QFile file(path); require(file.open(QIODevice::ReadOnly), "thumbnail payload missing");
  const QByteArray data = file.readAll();
  require(data.size() >= ShellThumbnail::HeaderSize, "thumbnail header missing");
  Pixels result;
  require(ShellThumbnail::dimensions(reinterpret_cast<const unsigned char *>(data.constData()),
              result.width, result.height), "thumbnail dimensions invalid");
  require(data.size() == ShellThumbnail::HeaderSize + result.width * result.height * 4,
          "thumbnail payload length invalid");
  result.bytes.assign(data.begin() + ShellThumbnail::HeaderSize, data.end());
  return result;
}
Pixels bitmap(HBITMAP handle) {
  DIBSECTION data{};
  require(GetObjectW(handle, sizeof(data), &data) == sizeof(data) && data.dsBm.bmBits,
          "COM thumbnail is not a DIB section");
  require(data.dsBm.bmBitsPixel == 32 && data.dsBm.bmWidth > 0 && data.dsBm.bmHeight > 0,
          "COM thumbnail bitmap format invalid");
  Pixels result;
  result.width = data.dsBm.bmWidth; result.height = data.dsBm.bmHeight;
  const auto *bytes = static_cast<const unsigned char *>(data.dsBm.bmBits);
  for (unsigned y = 0; y < result.height; ++y)
    result.bytes.insert(result.bytes.end(), bytes + y * data.dsBm.bmWidthBytes,
                        bytes + y * data.dsBm.bmWidthBytes + result.width * 4);
  return result;
}
void artwork(const Pixels &pixels) {
  int ink = 0, visible = 0;
  unsigned minAlpha = 255, maxAlpha = 0;
  for (size_t i = 0; i < pixels.bytes.size(); i += 4) {
    if (pixels.bytes[i+3]) ++visible;
    minAlpha = (std::min)(minAlpha, unsigned(pixels.bytes[i+3]));
    maxAlpha = (std::max)(maxAlpha, unsigned(pixels.bytes[i+3]));
    if (pixels.bytes[i] < 200 && pixels.bytes[i+1] < 200 &&
        pixels.bytes[i+2] < 200 && pixels.bytes[i+3] > 0) ++ink;
  }
  if (ink <= 20)
    std::cerr << "shell-thumbnail: pixels=" << pixels.width << 'x' << pixels.height
              << " visible=" << visible << " ink=" << ink << " alpha="
              << minAlpha << ".." << maxAlpha << std::endl;
  require(ink > 20, "thumbnail contains no rendered vector artwork");
}
Pixels cli(const QString &converter, const QString &input, const QString &output,
           unsigned size = 128) {
  QProcess process;
  process.start(converter, {QLatin1String(ShellThumbnail::Command), input,
                           QString::number(size), output});
  require(process.waitForStarted(5000), "thumbnail CLI did not start");
  if (!process.waitForFinished(ShellThumbnail::TimeoutMs + 5000)) {
    process.kill(); process.waitForFinished(2000);
    throw std::runtime_error("thumbnail CLI exceeded deadline");
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    std::cerr << process.readAllStandardError().constData()
              << process.readAllStandardOutput().constData();
    throw std::runtime_error("actual thumbnail CLI renderer failed");
  }
  if (qEnvironmentVariableIsSet("GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS"))
    std::cerr << process.readAllStandardError().constData()
              << process.readAllStandardOutput().constData();
  auto result = payload(output);
  require(result.width <= size && result.height <= size, "CLI exceeded requested thumbnail size");
  return result;
}
Pixels com(IClassFactory *factory, const QString &input, unsigned size = 128) {
  IInitializeWithFile *initialize = nullptr;
  require(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&initialize))),
          "real thumbnail COM class creation failed");
  IThumbnailProvider *provider = nullptr;
  HRESULT result = initialize->QueryInterface(IID_PPV_ARGS(&provider));
  if (SUCCEEDED(result)) result = initialize->Initialize(input.toStdWString().c_str(), STGM_READ);
  initialize->Release();
  require(SUCCEEDED(result) && provider, "real thumbnail COM initialization failed");
  HBITMAP image = nullptr; WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
  result = provider->GetThumbnail(size, &image, &alpha); provider->Release();
  require(SUCCEEDED(result) && image && alpha == WTSAT_ARGB,
          "actual Shell provider failed to render thumbnail");
  auto pixels = bitmap(image); DeleteObject(image); return pixels;
}
Pixels comWorker(FactoryEntry entry, const QString &input, unsigned size = 128) {
  Pixels pixels;
  std::exception_ptr failure;
  const DWORD mainThread = GetCurrentThreadId();
  std::thread worker([&] {
    bool initialized = false;
    IClassFactory *factory = nullptr;
    try {
      require(GetCurrentThreadId() != mainThread, "thumbnail fixture did not use a COM worker");
      initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
      require(initialized, "COM worker initialization failed");
      require(SUCCEEDED(entry(CLSID_ToonzThumbnailProvider, IID_PPV_ARGS(&factory))),
              "worker thumbnail class factory unavailable");
      pixels = com(factory,input,size);
    } catch (...) { failure = std::current_exception(); }
    if (factory) factory->Release();
    if (initialized) CoUninitialize();
  });
  worker.join();
  if (failure) std::rethrow_exception(failure);
  return pixels;
}
void same(const Pixels &a, const Pixels &b) {
  require(a.width == b.width && a.height == b.height && a.bytes == b.bytes,
          "COM thumbnail differs from actual CLI renderer");
}
QString levelFile(const QDir &directory, bool envelope) {
  const QString suffix = QString(QChar(0x52d5)) + QChar(0x753b) + ".pli";
  const QString path = directory.filePath(
      QString(envelope ? "envelope " : "ordinary ") + suffix);
  TPaletteP palette = new TPalette;
  TVectorImageP image = new TVectorImage; image->setPalette(palette.getPointer());
  auto *stroke = new TStroke(std::vector<TThickPoint>{{-50,0,8},{0,30,8},{50,0,8}});
  stroke->setStyle(1);
  if (envelope) stroke->setOpacityProfile({{0.,.15},{1.,.85}});
  image->addStroke(stroke);
  TLevelWriterP writer{TFilePath(path)};
  writer->setPalette(palette.getPointer());
  writer->getFrameWriter(TFrameId(1))->save(image);
  return path;
}
QString sceneFile(const QDir &directory, const QString &levelPath, const QString &name,
                  bool sceneRelative = false) {
  const QString path = directory.filePath(name + ".tnz");
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  scene.setScenePath(TFilePath(path));
  scene.getCurrentCamera()->setSize(TDimensionD(2.,1.5));
  scene.getCurrentCamera()->setRes(TDimension(160,120));
  scene.getProperties()->setBgColor(TPixel32::White);
  auto *level = scene.createNewLevel(PLI_XSHLEVEL,L"thumbnail drawing")->getSimpleLevel();
  const TFilePath assetPath = sceneRelative
      ? TFilePath("$scenefolder") + TFilePath(levelPath).getLevelNameW()
      : TFilePath(levelPath);
  level->setPath(assetPath, true); level->load();
  scene.getXsheet()->setCell(0,0,TXshCell(level,TFrameId(1)));
  scene.saveCopy(TFilePath(path));
  // Validate persisted input independently of the icon loader and its project
  // discovery. A default-sized white thumbnail must never count as a scene.
  ToonzScene reopened;
  reopened.loadTnzFile(TFilePath(path));
  reopened.loadResources(false);
  const TXshCell cell = reopened.getXsheet()->getCell(0,0);
  auto *savedLevel = cell.getSimpleLevel();
  require(reopened.getCurrentCamera()->getRes() == TDimension(160,120) &&
          reopened.getCurrentCamera()->getSize() == TDimensionD(2.,1.5) &&
          savedLevel && cell.m_frameId == TFrameId(1) &&
          savedLevel->getPath() == assetPath &&
          reopened.decodeFilePath(savedLevel->getPath()) == TFilePath(levelPath),
          "saved thumbnail scene lost camera, cell or resource path");
  const TVectorImageP savedImage = savedLevel->getFrame(TFrameId(1),false);
  require(savedImage && savedImage->getPalette() && savedImage->getStrokeCount() == 1 &&
          savedImage->getStroke(0)->getStyle() == 1 &&
          savedImage->getPalette()->getStyle(1)->getMainColor() == TPixel32::Black &&
          savedImage->getBBox().getLx() > 100.,
          "saved thumbnail scene lost its native vector resource");
  std::cout << "shell-thumbnail: verified saved scene camera160x120/cell/resource; relative="
            << sceneRelative << std::endl;
  return path;
}
int cancelPoll = 0;
bool cancelAfterLaunch() { return ++cancelPoll > 1; }
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  const auto args = application.arguments();
  // A real child process for timeout, cancellation and malformed-result tests.
  // This branch belongs only to the test executable, never to tconverter.
  if (args.size() == 5 && args.at(1) == QLatin1String(ShellThumbnail::Command)) {
    QFile partial(args.at(4));
    if (!partial.open(QIODevice::WriteOnly)) return 2;
    auto bytes = ShellThumbnail::header(ShellThumbnail::MaxSide + 1, 1);
    partial.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); partial.close();
    if (QFileInfo(args.at(2)).baseName() == "stub_hang") { Sleep(60000); return 0; }
    return 0;
  }
  if (args.size() != 3) return 2;
  try {
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM initialization failed");
    TThread::init(); TEnv::setRootVarName("TOONZROOT"); TEnv::setSystemVarPrefix("TOONZ");
    TEnv::setApplicationFileName(QCoreApplication::applicationFilePath().toStdString());
    TEnv::initUserStuffDir(); Tiio::defineStd(); initImageIo();
    QTemporaryDir workspace;
    require(workspace.isValid(), "thumbnail fixture directory missing");
    QDir root(workspace.path()); require(root.mkdir("request-temp"), "request temporary directory missing");
    const auto temp = root.filePath("request-temp").toStdWString();
    SetEnvironmentVariableW(L"TMP", temp.c_str()); SetEnvironmentVariableW(L"TEMP", temp.c_str());
    HMODULE module = LoadLibraryW(args.at(2).toStdWString().c_str());
    require(module, "actual thumbnail provider DLL failed to load");
    const auto getClass = reinterpret_cast<FactoryEntry>(GetProcAddress(module,"DllGetClassObject"));
    require(getClass, "provider has no declared DllGetClassObject export");
    const QString ordinary = levelFile(root,false), envelope = levelFile(root,true);
    const QString ordinaryScene = sceneFile(root,ordinary,"ordinary scene");
    const QString envelopeScene = sceneFile(root,envelope,"envelope scene");
    const QString relativeScene = sceneFile(root,ordinary,"relative scene",true);
    const QString relativeEnvelopeScene = sceneFile(root,envelope,"relative envelope scene",true);
    std::vector<Pixels> rendered;
    int index = 0;
    for (const QString &input : {ordinary,envelope,ordinaryScene,envelopeScene,
                                relativeScene,relativeEnvelopeScene}) {
      std::cout << "shell-thumbnail: rendering " << QFileInfo(input).fileName().toStdString() << std::endl;
      auto expected = cli(args.at(1),input,root.filePath(QString("render-%1.bin").arg(index++)));
      if (QFileInfo(input).suffix() == "tnz")
        require(expected.width == 128 && expected.height == 96,
                "scene thumbnail did not retain the saved camera aspect ratio");
      artwork(expected); same(expected, comWorker(getClass,input)); rendered.push_back(expected);
      require(!qobject_cast<QGuiApplication *>(QCoreApplication::instance()),
              "COM provider upgraded the host to a GUI application");
      require(QDir(QString::fromStdWString(temp)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
              "successful COM request leaked temporary output");
    }
    require(rendered[0].bytes != rendered[1].bytes && rendered[2].bytes != rendered[3].bytes,
            "PLI or TNZ thumbnails lost the stroke opacity envelope");
    require(rendered[4].bytes != rendered[5].bytes,
            "scene-relative TNZ thumbnails lost the stroke opacity envelope");
    const auto tiny = comWorker(getClass,envelope,1);
    require(tiny.width == 1 && tiny.height == 1, "tiny Shell request exceeded dimensions");
    const auto executable = QCoreApplication::applicationFilePath().toStdWString();
    HBITMAP failed = nullptr;
    ThumbnailProcess::Options options; options.timeoutMs = 250;
    const auto started = GetTickCount64();
    require(ThumbnailProcess::render(executable,L"stub_hang.pli",64,&failed,options) ==
                HRESULT_FROM_WIN32(ERROR_TIMEOUT) && !failed,
            "hung renderer was not stopped at the deadline");
    require(GetTickCount64() - started < 5000, "thumbnail timeout was not bounded");
    options.timeoutMs = 10000; options.isCanceled = cancelAfterLaunch; cancelPoll = 0;
    require(ThumbnailProcess::render(executable,L"stub_hang.pli",64,&failed,options) ==
                HRESULT_FROM_WIN32(ERROR_CANCELLED) && !failed,
            "canceled renderer was not terminated");
    options.isCanceled = nullptr;
    require(FAILED(ThumbnailProcess::render(executable,L"stub_bad.pli",64,&failed,options)) && !failed,
            "oversized renderer payload reached the Shell");
    require(FAILED(ThumbnailProcess::render(L"missing-renderer.exe",ordinary.toStdWString(),64,&failed)) && !failed,
            "missing renderer was not reported");
    require(QDir(QString::fromStdWString(temp)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
            "failed, canceled or timed-out request leaked temporary files");
    FreeLibrary(module); CoUninitialize();
    std::cout << "shell-thumbnail: PASS (ordinary/envelope PLI/TNZ, actual CLI and COM, bounded cleanup)" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "shell-thumbnail: " << error.what() << std::endl; return 1;
  } catch (...) {
    std::cerr << "shell-thumbnail: native exception" << std::endl; return 1;
  }
}
