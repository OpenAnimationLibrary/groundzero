#pragma once

#ifndef SCRIPTBINDING_H
#define SCRIPTBINDING_H

#include <QObject>
#include <QJSValue>
#include <QJSEngine>
#include <QHash>
#include <QMetaType>
#include <QColor>

#include <functional>

#include "timage.h"
#include "toonz/vectorizerparameters.h"
#include "toonz/txshsimplelevel.h"
#include "tlevel.h"
#include "tgeometry.h"

#include "tcommon.h"

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

// Macro for standard wrapper methods
//
// ctor() implements the script constructor of the class (e.g. new Image()).
// It receives the arguments of the script call; to make the script throw it
// calls engine->throwError() and returns QJSValue().
#define WRAPPER_STD_METHODS(T)                                                 \
  static QJSValue ctor(QJSEngine *engine, const QJSValueList &args);

// Macro for standard constructor implementation
#define WRAPPER_STD_CTOR_IMPL(T)                                               \
  QJSValue T::ctor(QJSEngine *engine, const QJSValueList &args) {              \
    return TScriptBinding::wrap(engine, new T());                              \
  }

// Forward declarations
class TAffine;
class TLevelReader;
class ToonzScene;
class TFilePath;

namespace TScriptBinding {

// A native script function (see Host::defineFunction()).
typedef std::function<QJSValue(QJSEngine *engine, const QJSValueList &args)>
    NativeFunction;

// Returns the script value of obj, which becomes owned by the engine (unless
// it has a parent). Script objects of the bound classes get the prototype of
// their script constructor, so that e.g. "" + image calls Image::toString()
// and image instanceof Image is true.
DVAPI QJSValue wrap(QJSEngine *engine, QObject *obj);
// The same, from a method of caller, which must not be deleted meanwhile
DVAPI QJSValue wrap(QJSEngine *engine, QObject *obj, const QObject *caller);

// Returns the object wrapped by value, if it is a T (else 0)
template <class T>
T *cast(const QJSValue &value) {
  return qobject_cast<T *>(value.toQObject());
}

class DVAPI Void final : public QObject {
  Q_OBJECT
public:
  WRAPPER_STD_METHODS(Void)
};

class DVAPI Wrapper : public QObject {
  Q_OBJECT

  static int m_count;
  int m_id;

public:
  Wrapper();
  virtual ~Wrapper();

  Q_PROPERTY(int id READ getId)
  int getId() const { return m_id; }

  void print(const QString &msg);
  void print(const QJSValueList &lst);

  void warning(const QString &msg);

  template <class T>
  QJSValue create(QJSEngine *engine) {
    return create(engine, static_cast<T *>(this));
  }

  template <class T>
  static QJSValue create(QJSEngine *engine, T *obj) {
    return wrap(engine, obj);
  }

protected:
  template <class T>
  QJSValue create(T *obj) const {
    return wrap(engine(), obj, this);
  }

  // The engine running the script that is calling this object (the object
  // must have been wrapped, see wrap())
  QJSEngine *engine() const { return qjsEngine(this); }

  // The script value of this object (what the script methods return to
  // allow chaining, e.g. image.load(path).save(otherPath))
  QJSValue self() const;

  // Makes the calling script throw an Error with the given message. Returns
  // QJSValue() (undefined): methods can simply "return throwError(...)".
  // Nothing (e.g. QJSValue::toString()) must be called on the engine after
  // it in the same native call.
  QJSValue throwError(const QString &message) const;
};

// Native side of the JavaScript glue installed by bindAll(). QJSEngine can't
// wrap C++ function pointers as script functions, so the global constructors
// (Image, Level, ...) and functions (print, run, ...) are small JavaScript
// closures that pass their arguments, as an array, to the Q_INVOKABLE methods
// of the Host. Scripts never get a reference to the Host itself.
class Host final : public QObject {
  Q_OBJECT
  QJSEngine *m_engine;
  QJSValue m_glue;
  QHash<const QMetaObject *, QJSValue> m_prototypes;
  QHash<QString, NativeFunction> m_functions;

public:
  explicit Host(QJSEngine *engine);
  ~Host();

  // The Host of engine (created by bindAll()), or 0
  static Host *instance(QJSEngine *engine);

  void install();

  // Defines (or replaces) the global script function name
  void defineFunction(const QString &name, const NativeFunction &f);
  // Defines className.prototype.name, and className.name with the same
  // function
  void defineMethod(const QString &className, const QString &name,
                    const NativeFunction &f);

  // The prototype of the script objects wrapping a metaObject instance
  QJSValue getPrototype(const QMetaObject *metaObject) const;

  Q_INVOKABLE QJSValue construct(const QString &className,
                                 const QJSValue &args);
  Q_INVOKABLE QJSValue call(const QString &name, const QJSValue &args);
  Q_INVOKABLE QString describe(const QJSValue &value) const;
};

// Initialize all script bindings
DVAPI void bindAll(QJSEngine &engine);

// Defines (or replaces) the global script function name (bindAll() must have
// been called)
DVAPI void defineFunction(QJSEngine &engine, const QString &name,
                          const NativeFunction &f);

// The string printed for value: for the bound classes it is the result of
// their toString() method
DVAPI QString toString(const QJSValue &value);

// Helper functions
// They make the calling script throw an Error and return false if the check
// fails

// Check the number of arguments
DVAPI bool checkArgumentCount(QJSEngine *engine, const QJSValueList &args,
                              const QString &name, int minCount, int maxCount);
DVAPI bool checkArgumentCount(QJSEngine *engine, const QJSValueList &args,
                              const QString &name, int count);

// Check the color. if colorName is not valid then an error is thrown
DVAPI bool checkColor(QJSEngine *engine, const QString &colorName,
                      QColor &color);

}  // namespace TScriptBinding

// Declare meta-type for script binding
Q_DECLARE_METATYPE(TScriptBinding::Void *)

#endif  // SCRIPTBINDING_H
