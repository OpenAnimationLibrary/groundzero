#include "tests/plugin_storage_lifetime.h"

#include "plugin_parameter_storage.h"
#include "plugin_param_interface.h"
#include "pluginhost.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tspectrumparam.h"
#include "tmacrofx.h"
#include "toonzqt/fxsettings.h"
#include "toonzqt/paramfield.h"
#include "toonzqt/pluginloader.h"

#include <QComboBox>
#include <QPointer>
#include <QStackedWidget>

#include <array>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

extern "C" int set_parameter_pages_with_error(void*, int, toonz_param_page_t*,
                                              int*, void**);

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int failCopyAfter = -1;
struct CopyFailure {
  int previous = failCopyAfter;
  CopyFailure() { failCopyAfter = 0; }
  ~CopyFailure() { failCopyAfter = previous; }
};

struct Description {
  std::string suffix;
  std::array<std::string, 4> keys;
  std::array<std::string, 2> captions;
  std::array<const char*, 2> labels;
  std::string text;
  std::array<toonz_param_spectrum_t, 2> colors;
  std::array<toonz_param_desc_t, 4> parameters = {};
  std::array<toonz_param_group_t, 2> groups    = {};
  std::array<toonz_param_page_t, 2> pages      = {};

  explicit Description(bool newer)
      : suffix(newer ? "new" : "old")
      , captions{suffix + " first", suffix + " second"}
      , text(suffix + " text") {
    keys   = {"storage_choice_" + suffix, "storage_colors_" + suffix,
              "storage_text_" + suffix, "storage_number_" + suffix};
    labels = {captions[0].c_str(), captions[1].c_str()};
    colors = {{{0, 1, 0, 0, 1}, {1, 0, 0, 1, 1}}};
    for (size_t index = 0; index < parameters.size(); ++index) {
      parameters[index].base = {
          {1, 0}, TOONZ_PARAM_DESC_TYPE_PARAM, keys[index].c_str()};
      parameters[index].key  = keys[index].c_str();
      parameters[index].note = text.c_str();
    }
    parameters[0].traits_tag   = TOONZ_PARAM_TYPE_ENUM;
    parameters[0].traits.e     = {newer ? 0 : 1, 2, labels.data(), 0};
    parameters[1].traits_tag   = TOONZ_PARAM_TYPE_SPECTRUM;
    parameters[1].traits.g     = {0, 2, colors.data(), 0};
    parameters[2].traits_tag   = TOONZ_PARAM_TYPE_STRING;
    parameters[2].traits.s.def = text.c_str();
    parameters[3].traits_tag   = TOONZ_PARAM_TYPE_INT;
    parameters[3].traits.i     = {newer ? 29 : 17, 0, 100, 0};
    groups[0]                  = {
        {{1, 0}, TOONZ_PARAM_DESC_TYPE_GROUP, "Main"}, 3, parameters.data()};
    groups[1] = {
        {{1, 0}, TOONZ_PARAM_DESC_TYPE_GROUP, "Extra"}, 1, &parameters[3]};
    pages[0] = {
        {{1, 0}, TOONZ_PARAM_DESC_TYPE_PAGE, "Parameters"}, 1, &groups[0]};
    pages[1] = {
        {{1, 0}, TOONZ_PARAM_DESC_TYPE_PAGE, "Additional"}, 1, &groups[1]};
  }
};

TFxP createHost() {
  TFxP host = PluginLoader::create_host("_plg_fixture_single_probe");
  require(host, "Storage fixture could not create its plugin host");
  return host;
}

void checkEnumValues(const TFxP& host, const std::string& suffix,
                     int expected) {
  TIntEnumParamP choice(
      host->getParams()->getParam("storage_choice_" + suffix));
  require(choice && choice->getValue() == expected,
          "Generation lost enum value");
  int value = -1;
  std::string caption;
  choice->getItem(1, value, caption);
  require(value == 1 && caption == suffix + " second",
          "Generation lost copied enum text");
}

void checkScalarValues(const TFxP& host, const std::string& suffix,
                       int expected) {
  TStringParamP text(host->getParams()->getParam("storage_text_" + suffix));
  TIntParamP number(host->getParams()->getParam("storage_number_" + suffix));
  const std::wstring expectedText(suffix.begin(), suffix.end());
  require(text && text->getValue() == expectedText + L" text",
          "Generation lost copied string default");
  require(number && number->getValue() == expected,
          "Generation lost integer value");
}

void checkValues(const TFxP& host, bool newer) {
  const std::string suffix = newer ? "new" : "old";
  checkEnumValues(host, suffix, newer ? 0 : 1);
  checkScalarValues(host, suffix, newer ? 29 : 17);
  TSpectrumParamP spectrum(
      host->getParams()->getParam("storage_colors_" + suffix));
  require(spectrum && spectrum->getKeyCount() == 2,
          "Generation lost spectrum keys");
  const auto colors = spectrum->getValue(0);
  require(colors.getKey(0).second == TPixel32::Red &&
              colors.getKey(1).second == TPixel32::Blue,
          "Generation lost spectrum colors");
}

ParamsPageSet* currentPages(ParamViewer& viewer) {
  auto* stack =
      viewer.findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly);
  require(stack, "Storage fixture has no viewer stack");
  auto* pages = dynamic_cast<ParamsPageSet*>(stack->currentWidget());
  require(pages && pages->getParamsPageCount() == 3,
          "Plugin host cycles duplicated or lost settings pages");
  require(pages->findChildren<ParamField*>().size() == 4,
          "Plugin host cycles duplicated or lost parameter fields");
  return pages;
}

ParamField* choiceField(ParamsPageSet* pages, bool newer) {
  const QString key = newer ? "storage_choice_new" : "storage_choice_old";
  for (auto* field : pages->findChildren<ParamField*>())
    if (field->property("fxParamName").toString() == key) return field;
  throw std::runtime_error("Viewer reused a field from the wrong generation");
}

void checkChoice(ParamViewer& viewer, bool newer, int expected) {
  viewer.update(0, true);
  auto* field = choiceField(currentPages(viewer), newer);
  auto* combo = field->findChild<QComboBox*>();
  require(combo && combo->currentIndex() == expected,
          "Viewer retained a stale parameter binding");
}

struct DestructionTrace {
  int observed                         = 0;
  bool invalid                         = false;
  int (*original)(toonz_node_handle_t) = nullptr;
};
DestructionTrace* activeTrace = nullptr;

int inspectDestruction(toonz_node_handle_t node) {
  auto* host = reinterpret_cast<RasterFxPluginHost*>(node);
  for (const char* suffix : {"old", "new"}) {
    const std::string key = std::string("storage_choice_") + suffix;
    if (const Param* parameter = host->getParam(key.c_str())) {
      ++activeTrace->observed;
      const auto* description    = parameter->desc();
      const std::string expected = std::string(suffix) + " second";
      if (!description ||
          std::string(description->traits.e.array[1]) != expected)
        activeTrace->invalid = true;
    }
  }
  return activeTrace->original ? activeTrace->original(node) : TOONZ_OK;
}
}  // namespace

class PluginStorageLifetimeTest {
  using Generation = std::shared_ptr<const PluginParameterStorage>;
  struct Restore {
    PluginInformation* information;
    Generation original;
    DestructionTrace trace;
    explicit Restore(PluginInformation* value)
        : information(value), original(std::atomic_load(&value->parameters_)) {
      trace.original                 = information->handler_->destroy;
      activeTrace                    = &trace;
      information->handler_->destroy = inspectDestruction;
    }
    ~Restore() {
      information->handler_->destroy = trace.original;
      activeTrace                    = nullptr;
      std::atomic_store(&information->parameters_, original);
    }
  };

  static void publish(RasterFxPluginHost& setup, bool newer) {
    Description source(newer);
    int error      = 0;
    void* position = nullptr;
    require(set_parameter_pages_with_error(&setup, 2, source.pages.data(),
                                           &error, &position) == TOONZ_OK,
            "Valid descriptor generation was rejected");
    source.captions[1] = "source overwritten after publication";
    source.text        = "source overwritten after publication";
  }

  static void rejectStructure(RasterFxPluginHost& setup, Description& source,
                              int count) {
    int error      = 0;
    void* position = nullptr;
    require(set_parameter_pages_with_error(&setup, count, source.pages.data(),
                                           &error, &position) ==
                TOONZ_ERROR_INVALID_VALUE,
            "Malformed nested descriptor storage was accepted");
  }

  static void checkInvalidStructure(RasterFxPluginHost& setup) {
    Description source(true);
    for (int count : {-1, 32}) rejectStructure(setup, source, count);
    for (int count : {-1, 33}) {
      source.pages[0].num = count;
      rejectStructure(setup, source, 2);
    }
    source.pages[0].num   = 1;
    source.pages[0].array = nullptr;
    rejectStructure(setup, source, 2);
    source.pages[0].array = source.groups.data();
    for (int count : {-1, 65536}) {
      source.groups[0].num = count;
      rejectStructure(setup, source, 2);
    }
    source.groups[0].num   = 3;
    source.groups[0].array = nullptr;
    rejectStructure(setup, source, 2);
  }

  static void checkRollback(RasterFxPluginHost& setup,
                            PluginInformation& information) {
    const auto before = std::atomic_load(&information.parameters_);
    Description source(true);
    int error      = 0;
    void* position = nullptr;
    {
      CopyFailure allocationFailure;
      require(set_parameter_pages_with_error(&setup, 2, source.pages.data(),
                                             &error, &position) ==
                  TOONZ_ERROR_OUT_OF_MEMORY,
              "Staged allocation failure crossed the SDK exception boundary");
    }
    require(std::atomic_load(&information.parameters_) == before,
            "Failed staged replacement changed the published generation");
    checkInvalidStructure(setup);
    require(std::atomic_load(&information.parameters_) == before,
            "Invalid replacement changed the published generation");
  }

  static void checkCloneBorrowers(RasterFxPluginHost& setup,
                                  PluginInformation& information) {
    for (bool sourceFirst : {true, false}) {
      publish(setup, false);
      std::weak_ptr<const PluginParameterStorage> generation =
          std::atomic_load(&information.parameters_);
      TFxP source = createHost();
      TFxP clone  = source->clone(false);
      publish(setup, true);
      if (sourceFirst)
        source = TFxP();
      else
        clone = TFxP();
      require(!generation.expired(),
              "A surviving host or clone lost its descriptor lease");
      TFxP survivor = sourceFirst ? clone : source;
      checkValues(survivor, false);
      TFxP finalClone = survivor->clone(false);
      source          = TFxP();
      clone           = TFxP();
      survivor        = TFxP();
      require(!generation.expired(), "A final clone lost its descriptor lease");
      checkValues(finalClone, false);
      finalClone = TFxP();
      require(generation.expired(),
              "An unused descriptor generation survived its final clone");
    }
  }

  static void checkViewers(RasterFxPluginHost& setup,
                           PluginInformation& information) {
    publish(setup, false);
    std::weak_ptr<const PluginParameterStorage> oldGeneration =
        std::atomic_load(&information.parameters_);
    for (int cycle = 0; cycle < 32; ++cycle) checkValues(createHost(), false);
    TFxP older  = createHost();
    TFxP cloned = older->clone(false);
    checkValues(cloned, false);
    auto retainedViewer  = std::make_unique<ParamViewer>();
    auto switchingViewer = std::make_unique<ParamViewer>();
    retainedViewer->setFx(older, older, 0, nullptr);
    switchingViewer->setFx(older, older, 0, nullptr);
    QPointer<ParamsPageSet> oldPages = currentPages(*switchingViewer);
    QPointer<ParamField> oldField    = choiceField(oldPages, false);
    checkRollback(setup, information);
    checkValues(createHost(), false);
    checkValues(older, false);
    checkChoice(*retainedViewer, false, 1);
    publish(setup, true);
    TFxP newer = createHost();
    checkValues(newer, true);
    checkValues(older, false);
    TFxP lateClone = older->clone(false);
    checkValues(lateClone, false);
    switchingViewer->setFx(newer, newer, 0, nullptr);
    require(currentPages(*switchingViewer) != oldPages,
            "New layout reused old cached pages");
    checkChoice(*switchingViewer, true, 0);
    checkChoice(*retainedViewer, false, 1);
    switchingViewer->setFx(older, older, 0, nullptr);
    require(currentPages(*switchingViewer) == oldPages &&
                choiceField(oldPages, false) == oldField,
            "Returning to an old generation replaced live cached fields");
    TIntEnumParamP choice(older->getParams()->getParam("storage_choice_old"));
    choice->setValue(0);
    checkChoice(*retainedViewer, false, 0);
    checkChoice(*switchingViewer, false, 0);
    switchingViewer->setFx(newer, newer, 0, nullptr);
    older     = TFxP();
    cloned    = TFxP();
    lateClone = TFxP();
    require(!oldGeneration.expired(),
            "A cached viewer lost its descriptor generation");
    retainedViewer.reset();
    require(!oldGeneration.expired(),
            "Visited cached pages lost their descriptor generation");
    switchingViewer.reset();
    require(oldPages.isNull() && oldField.isNull() && oldGeneration.expired(),
            "Old descriptors or widgets survived their final viewer/host/clone "
            "owner");
  }

public:
  static void run() {
    TFxP originalHost = createHost();
    auto* host = static_cast<RasterFxPluginHost*>(originalHost.getPointer());
    Restore restore(host->pi_);
    RasterFxPluginHost setup(host->pi_);
    checkCloneBorrowers(setup, *host->pi_);
    checkViewers(setup, *host->pi_);
    require(restore.trace.observed >= 36 && !restore.trace.invalid,
            "Plugin destroy callback outlived its parameter generation");
  }
};

void checkpointPluginStorageCopyForTest() {
  if (failCopyAfter == 0) throw std::bad_alloc();
  if (failCopyAfter > 0) --failCopyAfter;
}

bool verifyPluginStorageLifetime(QString& failure) {
  try {
    PluginStorageLifetimeTest::run();
    return true;
  } catch (const std::exception& error) {
    failure = QString::fromUtf8(error.what());
    return false;
  }
}
