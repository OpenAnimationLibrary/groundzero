

#include "toonz/scriptbinding.h"

// TnzLib includes
#include "toonz/scriptbinding_scene.h"
#include "toonz/scriptbinding_stageobject.h"
#include "toonz/scriptbinding_image.h"
#include "toonz/scriptbinding_level.h"
#include "toonz/scriptbinding_files.h"
#include "toonz/scriptbinding_renderer.h"
#include "toonz/scriptbinding_outline_vectorizer.h"
#include "toonz/scriptbinding_centerline_vectorizer.h"
#include "toonz/scriptbinding_rasterizer.h"
#include "toonz/scriptbinding_toonz_raster_converter.h"
#include "toonz/scriptbinding_image_builder.h"
#include "toonz/tcenterlinevectorizer.h"
#include "toonz/tcamera.h"
#include "toonz/stage.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"

// TnzBase includes
#include "tlevel.h"
#include "trop.h"
#include "trasterimage.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"
#include "tpalette.h"
#include "tvectorrenderdata.h"
#include "tgeometry.h"
#include "tfiletype.h"
#include "tofflinegl.h"
#include "tlevel_io.h"

// Qt includes
#include <QJSEngine>
#include <QCoreApplication>
#include <QEvent>
#include <QColor>

namespace TScriptBinding {

//===========================================================================

WRAPPER_STD_CTOR_IMPL(Void)

//===========================================================================

int Wrapper::m_count = 0;

Wrapper::Wrapper() : m_id(++m_count) {}

Wrapper::~Wrapper() = default;

void Wrapper::print(const QString &msg) {
  QJSValueList lst;
  lst << msg;
  print(lst);
}

void Wrapper::print(const QJSValueList &lst) {
  QJSValue printFunc = engine()->globalObject().property("print");
  if (printFunc.isCallable()) {
    printFunc.call(lst);
  }
}

void Wrapper::warning(const QString &msg) {
  QJSValueList lst;
  lst << msg;
  QJSValue warningFunc = engine()->globalObject().property("warning");
  if (warningFunc.isCallable()) {
    warningFunc.call(lst);
  }
}

QJSValue Wrapper::self() const {
  return wrap(engine(), const_cast<Wrapper *>(this));
}

QJSValue Wrapper::throwError(const QString &message) const {
  engine()->throwError(message);
  return QJSValue();
}

//===========================================================================

bool checkArgumentCount(QJSEngine *engine, const QJSValueList &args,
                        const QString &name, int minCount, int maxCount) {
  int count = args.size();
  if (minCount <= count && count <= maxCount) return true;

  QString range;
  if (minCount != maxCount) {
    range = QObject::tr("%1-%2").arg(minCount).arg(maxCount);
  } else {
    range = QObject::tr("%1").arg(minCount);
  }

  engine->throwError(QObject::tr("Expected %1 argument(s) in %2, got %3")
                         .arg(range, name)
                         .arg(count));
  return false;
}

bool checkArgumentCount(QJSEngine *engine, const QJSValueList &args,
                        const QString &name, int count) {
  return checkArgumentCount(engine, args, name, count, count);
}

bool checkColor(QJSEngine *engine, const QString &colorName, QColor &color) {
  color.setNamedColor(colorName);
  if (!color.isValid()) {
    engine->throwError(
        QObject::tr("%1 is not a valid color (valid color names are 'red', "
                    "'transparent', '#FF8800', etc.)")
            .arg(colorName));
    return false;
  }
  return true;
}

//=============================================================================

namespace {

typedef QJSValue (*CtorFunction)(QJSEngine *engine, const QJSValueList &args);

struct ClassInfo {
  const char *m_name;
  const QMetaObject *m_metaObject;
  CtorFunction m_ctor;
};

template <class T>
ClassInfo classInfo(const char *name) {
  return {name, &T::staticMetaObject, &T::ctor};
}

// The script classes: name of the global constructor, C++ class
const ClassInfo *classes(int &count) {
  static const ClassInfo infos[] = {
      classInfo<Image>("Image"),
      classInfo<Level>("Level"),
      classInfo<Scene>("Scene"),
      classInfo<StageObject>("StageObject"),
      classInfo<Transform>("Transform"),
      classInfo<ImageBuilder>("ImageBuilder"),
      classInfo<OutlineVectorizer>("OutlineVectorizer"),
      classInfo<CenterlineVectorizer>("CenterlineVectorizer"),
      classInfo<Rasterizer>("Rasterizer"),
      classInfo<ToonzRasterConverter>("ToonzRasterConverter"),
      classInfo<FilePath>("FilePath"),
      classInfo<Renderer>("Renderer"),
  };
  count = sizeof(infos) / sizeof(infos[0]);
  return infos;
}

QJSValueList toList(const QJSValue &array) {
  QJSValueList list;
  quint32 length = array.property(QStringLiteral("length")).toUInt();
  for (quint32 i = 0; i < length; ++i) list.append(array.property(i));
  return list;
}

// The JavaScript side of the Host (see the comment of the class). It is
// called with the Host, the global object and the names of the script
// classes, and returns the functions used by Host to define the classes and
// the global functions.
const char *const glueSource = R"JS(
(function (host, global, classNames) {
  var slice = Array.prototype.slice;

  // Common prototype of all the script objects of the bound classes: string
  // conversions ("" + image, String(image), ...) use their C++ toString().
  var base = {};
  Object.defineProperty(base, Symbol.toPrimitive, {
    value: function (hint) { return host.describe(this); }
  });

  var prototypes = {};
  classNames.forEach(function (name) {
    var ctor = function () {
      return host.construct(name, slice.call(arguments));
    };
    ctor.prototype = Object.create(base);
    Object.defineProperty(ctor.prototype, "constructor",
                          { value: ctor, writable: true, configurable: true });
    prototypes[name] = ctor.prototype;
    global[name] = ctor;
  });

  function nativeFunction(key) {
    return function () { return host.call(key, slice.call(arguments)); };
  }

  return {
    prototypes: prototypes,
    defineFunction: function (name) {
      global[name] = nativeFunction(name);
    },
    defineMethod: function (className, name) {
      var f = nativeFunction(className + "." + name);
      prototypes[className][name] = f;
      global[className][name] = f;
    }
  };
})
)JS";

}  // namespace

//=============================================================================

Host::Host(QJSEngine *engine) : QObject(engine), m_engine(engine) {}

Host::~Host() {}

Host *Host::instance(QJSEngine *engine) {
  return engine
             ? engine->findChild<Host *>(QString(), Qt::FindDirectChildrenOnly)
             : 0;
}

void Host::install() {
  int count              = 0;
  const ClassInfo *infos = classes(count);
  QJSValue classNames    = m_engine->newArray(count);
  for (int i = 0; i < count; ++i)
    classNames.setProperty(i, QString::fromLatin1(infos[i].m_name));

  QJSValue glueFunction =
      m_engine->evaluate(QString::fromLatin1(glueSource), "scriptbinding.js");
  Q_ASSERT(glueFunction.isCallable());
  m_glue = glueFunction.call(QJSValueList()
                             << m_engine->newQObject(this)
                             << m_engine->globalObject() << classNames);
  Q_ASSERT(m_glue.isObject());

  QJSValue prototypes = m_glue.property("prototypes");
  for (int i = 0; i < count; ++i)
    m_prototypes.insert(infos[i].m_metaObject,
                        prototypes.property(infos[i].m_name));
}

void Host::defineFunction(const QString &name, const NativeFunction &f) {
  m_functions.insert(name, f);
  m_glue.property("defineFunction")
      .callWithInstance(m_glue, QJSValueList() << name);
}

void Host::defineMethod(const QString &className, const QString &name,
                        const NativeFunction &f) {
  m_functions.insert(className + "." + name, f);
  m_glue.property("defineMethod")
      .callWithInstance(m_glue, QJSValueList() << className << name);
}

QJSValue Host::getPrototype(const QMetaObject *metaObject) const {
  return m_prototypes.value(metaObject);
}

QJSValue Host::construct(const QString &className, const QJSValue &args) {
  int count              = 0;
  const ClassInfo *infos = classes(count);
  for (int i = 0; i < count; ++i)
    if (className == QLatin1String(infos[i].m_name))
      return infos[i].m_ctor(m_engine, toList(args));
  m_engine->throwError(tr("Unknown class %1").arg(className));
  return QJSValue();
}

QJSValue Host::call(const QString &name, const QJSValue &args) {
  auto it = m_functions.constFind(name);
  if (it == m_functions.constEnd()) {
    m_engine->throwError(tr("Unknown function %1").arg(name));
    return QJSValue();
  }
  // copy the function: it could redefine itself
  NativeFunction f = it.value();
  return f(m_engine, toList(args));
}

QString Host::describe(const QJSValue &value) const {
  // value can also be one of the prototypes (e.g. "" + Image.prototype)
  if (!value.isQObject()) return QStringLiteral("[object Object]");
  return TScriptBinding::toString(value);
}

//=============================================================================

QJSValue wrap(QJSEngine *engine, QObject *obj) {
  // The objects that the garbage collector finds unreachable are released
  // with deleteLater(). The script thread doesn't go back to its event loop
  // until the evaluation ends, so the deletions are done here: otherwise a
  // long script (e.g. processing all the frames of a level) would keep all
  // of its images in memory until it ends.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

  QJSValue value = engine->newQObject(obj);
  if (Host *host = Host::instance(engine)) {
    QJSValue prototype = host->getPrototype(obj->metaObject());
    if (prototype.isObject()) value.setPrototype(prototype);
  }
  return value;
}

void bindAll(QJSEngine &engine) {
  // Bind all scriptable classes
  Host *host = new Host(&engine);
  host->install();

  // Setup the "static" methods
  // ToonzRasterConverter.convert() is also a method of the instances
  host->defineMethod("ToonzRasterConverter", "convert",
                     &ToonzRasterConverter::convert);

  // Set version information in the script environment
  engine.globalObject().setProperty("ToonzVersion", "7.1");
}

void defineFunction(QJSEngine &engine, const QString &name,
                    const NativeFunction &f) {
  Host *host = Host::instance(&engine);
  Q_ASSERT(host);
  if (host) host->defineFunction(name, f);
}

QString toString(const QJSValue &value) {
  if (QObject *obj = value.toQObject()) {
    // Note: value.toString() can't be used, the engine would call its own
    // toString() for QObjects (or describe(), which calls this function)
    QString result;
    if (obj->metaObject()->indexOfMethod("toString()") >= 0 &&
        QMetaObject::invokeMethod(obj, "toString", Qt::DirectConnection,
                                  Q_RETURN_ARG(QString, result)))
      return result;
    return QString("%1(0x%2)")
        .arg(QString::fromLatin1(obj->metaObject()->className()))
        .arg((quintptr)obj, 0, 16);
  }
  return value.toString();
}

}  // namespace TScriptBinding
