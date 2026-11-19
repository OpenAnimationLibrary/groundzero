#include "harness.h"

#include "tools/tooltimer.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "toonz/studiopalette.h"
#include "toonz/toonzfolders.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "tenv.h"
#include "timagecache.h"
#include "tmetaimage.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tthread.h"
#include "ttoonzimage.h"
#include "tundo.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTextStream>

#include <cmath>

namespace brushstroke {

namespace {

const TDimension rasterSize(192, 128);
const double rasterDpi = 120.0;

// The tools read the time of every event from TToolTimer. A gesture is
// replayed with the time it was recorded at, on a clock that only moves
// forward: the input managers refuse a time before their last event.
const TTimerTicks ticksPerSecond = 1000000000;
TTimerTicks clockTicks           = 10 * ticksPerSecond;
TTimerTicks readClock() { return clockTicks; }

Gesture loadGesture(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly | QIODevice::Text),
          "cannot read gesture " + path.toStdString());
  Gesture gesture;
  QTextStream stream(&file);
  while (!stream.atEnd()) {
    const QString line = stream.readLine().trimmed();
    if (line.isEmpty() || line.startsWith('#')) continue;
    const QStringList fields = line.split(' ');
    require(fields.size() == 4,
            "malformed gesture line in " + path.toStdString());
    bool ok[4];
    const Sample sample = {
        fields[0].toDouble(&ok[0]), fields[1].toDouble(&ok[1]),
        fields[2].toDouble(&ok[2]), fields[3].toDouble(&ok[3])};
    require(ok[0] && ok[1] && ok[2] && ok[3],
            "malformed gesture number in " + path.toStdString());
    gesture.push_back(sample);
  }
  require(gesture.size() >= 2, "gesture too short: " + path.toStdString());
  return gesture;
}

TImage::Type imageType(int levelType) {
  switch (levelType) {
  case PLI_XSHLEVEL:
    return TImage::VECTOR;
  case TZP_XSHLEVEL:
    return TImage::TOONZ_RASTER;
  case OVL_XSHLEVEL:
    return TImage::RASTER;
  default:
    throw std::runtime_error("fixture level type has no brush");
  }
}

template <class Property>
Property* typed(TProperty* property, const char* name) {
  Property* result = dynamic_cast<Property*>(property);
  require(result, std::string("tool option has another type: ") + name);
  return result;
}

}  // namespace

//-----------------------------------------------------------------------------

Application::Application() {
  scene.setScene(new ToonzScene);
  sheet.setXsheet(scene.getScene()->getXsheet());
  frame.setFrame(0);
  column.setColumnIndex(0);
  object.setObjectId(TStageObjectId::ColumnId(0));
  TTool::setApplication(this);
}

Application::~Application() {
  TUndoManager::manager()->reset();
  level.setLevel(nullptr);
  sheet.setXsheet(nullptr);
  scene.setScene(nullptr);
  TTool::setApplication(nullptr);
}

//-----------------------------------------------------------------------------

void initializeProfile(const QString& root, const char* executable) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     root + "/settings");
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                     root + "/settings");
  TEnv::setApplicationFileName(executable);
  TEnv::setRootVarName("TOONZROOT");
  TEnv::setSystemVarPrefix("TOONZ");
  TEnv::setArgPathValue("TOONZROOT", root.toStdString());
  for (const auto& entry : TEnv::getSystemPathMap()) {
    const QString path =
        QDir(root).filePath(QString::fromStdString(entry.second));
    require(QDir().mkpath(path), "cannot create the private profile");
    TEnv::setArgPathValue("TOONZ" + entry.first, path.toStdString());
  }
  require(QDir().mkpath(root + "/cache"), "cannot create the private cache");
  TImageCache::instance()->setRootDir(
      TFilePath((root + "/cache").toStdWString()));
  StudioPalette::enable(false);
  TThread::init();
  // Every stroke asks for a new level icon, rendered on a worker thread with
  // an OpenGL context the offscreen platform does not have. No case reads an
  // icon, and a render still running when the test exits crashes it. After
  // shutdown() the executors accept the requests and never run them.
  TThread::shutdown();
}

//-----------------------------------------------------------------------------

TPaletteP makePalette() {
  TPaletteP palette        = new TPalette;
  TPalette::Page* page     = palette->getPage(0);
  const TPixel32 colours[] = {TPixel32(200, 30, 30), TPixel32(30, 60, 200),
                              TPixel32(20, 150, 60)};
  for (const TPixel32& colour : colours) palette->addStyle(colour);
  // Styles 0 and 1 are on the page already.
  page->insertStyle(1, 3);
  page->addStyle(2);
  page->addStyle(4);
  require(page->getStyleId(1) == 3 && page->getStyleId(2) == 1 &&
              page->getStyleId(3) == 2 && page->getStyleId(4) == 4,
          "fixture palette order is not 3, 1, 2, 4");
  return palette;
}

//-----------------------------------------------------------------------------

Fixture::Fixture(Application& application, const QString& gestureDir)
    : app(application), m_gestureDir(gestureDir) {
  TToolTimer::setTickSource(&readClock);
  Preferences* preferences = Preferences::instance();
  preferences->setValue(EnableAutocreation, true, false);
  preferences->setValue(EnableCreationInHoldCells, false, false);
  preferences->setValue(guidedDrawingType, 0, false);
  preferences->setValue(guidedAutoInbetween, false, false);
  preferences->setValue(vectorSnappingTarget, 0, false);
  preferences->setValue(useCtrlAltToResizeBrush, false, false);
}

Fixture::~Fixture() {
  if (m_tool) m_tool->setViewer(nullptr);
  TToolTimer::setTickSource(nullptr);
}

void Fixture::begin() {
  // Leaving the tool ends anything a previous case left unfinished, and the
  // next useTool() activates it again as a tool switch does.
  app.tool.setTool("T_Hand");
  m_tool = nullptr;
  TUndoManager::manager()->reset();
  app.level.setLevel(nullptr);
  TXsheet* xsheet = app.sheet.getXsheet();
  while (xsheet->getColumnCount() > 0) xsheet->removeColumn(0);
  m_level           = TXshSimpleLevelP();
  m_column          = -1;
  m_rows            = 0;
  viewer.pixelSize  = 1.0;
  viewer.viewMatrix = TAffine();
  viewer.hGuides.clear();
  viewer.vGuides.clear();
  app.frame.setFrame(0);
  app.column.setColumnIndex(0);
}

//-----------------------------------------------------------------------------

TXshSimpleLevel* Fixture::newLevel(int type, const wchar_t* name) {
  ToonzScene* scene      = app.scene.getScene();
  TXshSimpleLevelP level = new TXshSimpleLevel;
  const int number       = ++m_levelNumber;
  level->setType(type);
  level->setName(name + std::to_wstring(number));
  level->setScene(scene);
  level->setPath(TFilePath("+drawings/fixture" + std::to_string(number) +
                           (type == PLI_XSHLEVEL   ? ".pli"
                            : type == TZP_XSHLEVEL ? ".tlv"
                            : type == OVL_XSHLEVEL ? ".png"
                                                   : ".tzm")));
  if (type != META_XSHLEVEL) level->setPalette(makePalette().getPointer());
  if (type == TZP_XSHLEVEL || type == OVL_XSHLEVEL) {
    level->getProperties()->setImageRes(rasterSize);
    level->getProperties()->setImageDpi(TPointD(rasterDpi, rasterDpi));
    level->getProperties()->setDpi(rasterDpi);
  }
  scene->getLevelSet()->insertLevel(level.getPointer());
  return level.getPointer();
}

void Fixture::place(const TImageP& image) {
  const TFrameId fid(m_rows + 1);
  m_level->setFrame(fid, image);
  app.sheet.getXsheet()->setCell(m_rows, m_column,
                                 TXshCell(m_level.getPointer(), fid));
  ++m_rows;
}

void Fixture::addDrawing() {
  require(m_level, "addDrawing needs a level");
  TImageP image = m_level->createEmptyFrame();
  if (TVectorImageP vector = image) vector->setPalette(m_level->getPalette());
  if (TToonzImageP toonz = image) toonz->setPalette(m_level->getPalette());
  if (TRasterImageP raster = image) raster->setPalette(m_level->getPalette());
  place(image);
}

TXshSimpleLevel* Fixture::newVectorLevel() {
  m_level  = newLevel(PLI_XSHLEVEL, L"vector");
  m_column = app.sheet.getXsheet()->getColumnCount();
  m_rows   = 0;
  addDrawing();
  goToRow(0);
  return m_level.getPointer();
}

TXshSimpleLevel* Fixture::newToonzRasterLevel() {
  m_level  = newLevel(TZP_XSHLEVEL, L"toonzraster");
  m_column = app.sheet.getXsheet()->getColumnCount();
  m_rows   = 0;
  addDrawing();
  goToRow(0);
  return m_level.getPointer();
}

TXshSimpleLevel* Fixture::newFullColorLevel() {
  m_level  = newLevel(OVL_XSHLEVEL, L"fullcolor");
  m_column = app.sheet.getXsheet()->getColumnCount();
  m_rows   = 0;
  addDrawing();
  goToRow(0);
  return m_level.getPointer();
}

void Fixture::goToRow(int row) {
  app.column.setColumnIndex(m_column);
  app.object.setObjectId(TStageObjectId::ColumnId(m_column));
  app.frame.setFrame(row);
  app.level.setLevel(m_level.getPointer());
  TPaletteHandle* handle = app.palette.getCurrentLevelPalette();
  const int style        = handle->getPalette() == m_level->getPalette()
                               ? handle->getStyleIndex()
                               : 1;
  handle->setPalette(m_level->getPalette(), style);
  app.palette.editLevelPalette();
  if (m_tool) app.tool.onImageChanged(imageType(m_level->getType()));
}

void Fixture::setStyle(int styleId) {
  app.palette.getCurrentLevelPalette()->setStyleIndex(styleId);
}

void Fixture::addMetaColumn(const std::vector<TMetaObjectP>& objects) {
  TXshSimpleLevel* level = newLevel(META_XSHLEVEL, L"assistants");
  TMetaImage* image      = new TMetaImage;
  {
    TMetaImage::Writer writer(*image);
    for (const TMetaObjectP& object : objects) writer->push_back(object);
  }
  level->setFrame(TFrameId(1), TImageP(image));
  TXsheet* xsheet  = app.sheet.getXsheet();
  const int column = xsheet->getColumnCount();
  for (int row = 0; row < 4; ++row)
    xsheet->setCell(row, column, TXshCell(level, TFrameId(1)));
}

std::wstring Fixture::addBrushTip() {
  const QString folder =
      ToonzFolder::getLibraryFolder().getQString() + "/brushtips";
  require(QDir().mkpath(folder), "cannot create the brush tips folder");
  // An opaque image: black is paint. A slanted bar over a faint texture.
  QImage tip(24, 12, QImage::Format_RGB32);
  for (int y = 0; y < tip.height(); ++y)
    for (int x = 0; x < tip.width(); ++x) {
      const bool bar  = x >= 2 * y && x < 2 * y + 6;
      const int value = bar ? 0 : 200 + (x * 7 + y * 13) % 56;
      tip.setPixel(x, y, qRgb(value, value, value));
    }
  require(tip.save(folder + "/bar.png", "PNG"), "cannot write the brush tip");
  return L"bar.png";
}

TImageP Fixture::image(int row) const {
  const int actual = row < 0 ? app.frame.getFrame() : row;
  return app.sheet.getXsheet()->getCell(actual, m_column).getImage(false);
}

//-----------------------------------------------------------------------------

TTool* Fixture::useTool(const char* name) {
  require(m_level, "useTool needs a level");
  app.tool.onImageChanged(imageType(m_level->getType()));
  app.tool.setTool(name);
  m_tool = app.tool.getTool();
  require(m_tool && m_tool->getName() == name,
          std::string("tool is not registered: ") + name);
  m_tool->setViewer(&viewer);
  m_tool->onEnter();
  return m_tool;
}

TProperty* Fixture::property(const char* name) const {
  require(m_tool, "no tool is in use");
  for (int group = 0; group < 2; ++group) {
    TPropertyGroup* properties = m_tool->getProperties(group);
    if (!properties) continue;
    if (TProperty* found = properties->getProperty(name)) return found;
  }
  throw std::runtime_error(std::string("tool has no option named ") + name);
}

void Fixture::changed(const char* name) { m_tool->onPropertyChanged(name); }

void Fixture::setBool(const char* name, bool value) {
  typed<TBoolProperty>(property(name), name)->setValue(value);
  changed(name);
}

void Fixture::setNumber(const char* name, double value) {
  TProperty* option = property(name);
  if (TIntProperty* whole = dynamic_cast<TIntProperty*>(option))
    whole->setValue(int(value));
  else
    typed<TDoubleProperty>(option, name)->setValue(value);
  changed(name);
}

void Fixture::setEnum(const char* name, int index) {
  typed<TEnumProperty>(property(name), name)->setIndex(index);
  changed(name);
}

void Fixture::setEnumValue(const char* name, const std::wstring& value) {
  TEnumProperty* option = typed<TEnumProperty>(property(name), name);
  require(option->isValue(value),
          std::string("tool option has no such value: ") + name);
  option->setValue(value);
  changed(name);
}

void Fixture::setPair(const char* name, double first, double second) {
  TProperty* option = property(name);
  if (TIntPairProperty* whole = dynamic_cast<TIntPairProperty*>(option))
    whole->setValue(TIntPairProperty::Value(int(first), int(second)));
  else
    typed<TDoublePairProperty>(option, name)
        ->setValue(TDoublePairProperty::Value(first, second));
  changed(name);
}

//-----------------------------------------------------------------------------

const Gesture& Fixture::gesture(const char* name) {
  for (const auto& entry : m_gestures)
    if (entry.first == name) return entry.second;
  m_gestures.emplace_back(name,
                          loadGesture(m_gestureDir + "/" + name + ".txt"));
  return m_gestures.back().second;
}

void Fixture::play(const char* name, const Play& how) {
  require(m_tool, "play needs a tool");
  const Gesture& samples  = gesture(name);
  const TTimerTicks start = clockTicks + ticksPerSecond;
  TMouseEvent event;
  event.m_isTablet = how.tablet;
  event.setModifiers(how.shift, how.alt, how.ctrl);
  auto at = [&](const Sample& sample) {
    clockTicks = start + TTimerTicks(std::llround(sample.tMs * 1e6));
    const TPointD position =
        TPointD(sample.x * how.scale, sample.y * how.scale) + how.offset;
    event.m_pos      = position;
    event.m_mousePos = QPointF(position.x, position.y);
    event.m_pressure = how.tablet ? sample.pressure : 1.0;
    return position;
  };
  // The calls SceneViewer makes for a press, its moves and the release. It
  // presses whatever preLeftButtonDown() returns.
  m_tool->mouseMove(at(samples.front()), event);
  m_tool->preLeftButtonDown();
  event.m_button  = Qt::LeftButton;
  event.m_buttons = Qt::LeftButton;
  app.tool.setToolBusy(true);
  m_tool->setCanUndo(false);
  m_tool->leftButtonDown(at(samples.front()), event);
  for (size_t i = 1; i + 1 < samples.size(); ++i)
    m_tool->leftButtonDrag(at(samples[i]), event);
  event.m_buttons = Qt::NoButton;
  m_tool->leftButtonUp(at(samples.back()), event);
  app.tool.setToolBusy(false);
  m_tool->setCanUndo(true);
}

}  // namespace brushstroke
