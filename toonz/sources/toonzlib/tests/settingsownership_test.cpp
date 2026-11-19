// SPDX-License-Identifier: BSD-3-Clause

// Every scene owns cleanup parameters and output properties, so whatever they
// fail to release leaks once per scene load and render. The standard cleanup
// palette used to come with a reference nobody released, and asking the
// output properties for a format's properties used to replace the stored
// group with a new one each time without deleting the old one. This test
// checks that each palette and group is held once, freed with its owner, and
// that the format properties a caller edits stay the stored ones.

#include "toonz/cleanupparameters.h"
#include "tiio.h"
#include "toutputproperties.h"
#include "tpalette.h"
#include "tproperty.h"

#include <QCoreApplication>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

const char* const kFormat = "gzownership";

// Counts the groups alive: a writer's properties are copied with clone()
class CountedGroup final : public TPropertyGroup {
public:
  static int s_alive;

  CountedGroup() { ++s_alive; }
  ~CountedGroup() override { --s_alive; }

  TPropertyGroup* clone() const override {
    auto* group = new CountedGroup();
    auto* self  = const_cast<CountedGroup*>(this);
    for (int i = 0; i < getPropertyCount(); ++i)
      group->add(self->getProperty(i)->clone());
    return group;
  }
};

int CountedGroup::s_alive = 0;

void defineWriter() {
  auto* writer = new CountedGroup();
  writer->add(new TIntProperty("Quality", 0, 100, 90));
  auto* mode = new TEnumProperty("Mode");
  mode->addValue(L"Fast");
  mode->addValue(L"Best");
  writer->add(mode);
  auto* palette = new TBoolProperty("Palette", false);
  palette->setVisible(false);
  writer->add(palette);
  Tiio::defineWriterProperties(kFormat, writer);
}

TIntProperty* quality(TPropertyGroup* group) {
  auto* property = dynamic_cast<TIntProperty*>(group->getProperty("Quality"));
  require(property, "the format properties lost Quality");
  return property;
}

TEnumProperty* mode(TPropertyGroup* group) {
  auto* property = dynamic_cast<TEnumProperty*>(group->getProperty("Mode"));
  require(property, "the format properties lost Mode");
  return property;
}

void cleanupPaletteIsHeldOnce() {
  TPaletteP held;
  {
    CleanupParameters params;
    require(params.m_cleanupPalette->getRefCount() == 1,
            "a new cleanup palette carries a reference nobody releases");
    held = params.m_cleanupPalette;

    CleanupParameters copy(params);
    require(copy.m_cleanupPalette != held &&
                copy.m_cleanupPalette->getRefCount() == 1,
            "a copied cleanup palette is shared or held twice");

    TPaletteP replaced = copy.m_cleanupPalette;
    copy.assign(&params);
    require(replaced->getRefCount() == 1,
            "assigning a cloned palette kept a reference to the old one");

    TPalette* kept = copy.m_cleanupPalette.getPointer();
    copy.assign(&params, false);
    require(
        copy.m_cleanupPalette.getPointer() == kept && kept->getRefCount() == 1,
        "assigning without a clone replaced the palette");
  }
  require(held->getRefCount() == 1,
          "deleted cleanup parameters kept a reference to their palette");
  require(
      CleanupParameters::GlobalParameters.m_cleanupPalette->getRefCount() == 1,
      "the global cleanup palette is held twice");
}

void formatPropertiesStay() {
  {
    TOutputProperties out;
    TPropertyGroup* group = out.getFileFormatProperties(kFormat);
    quality(group)->setValue(42);
    for (int i = 0; i < 4; ++i)
      require(out.getFileFormatProperties(kFormat) == group,
              "asking again replaced the format properties a caller edits");
    require(quality(group)->getValue() == 42, "the edited value was lost");
    require(CountedGroup::s_alive == 2,
            "asking again for format properties left groups behind");

    {
      TOutputProperties copy(out);
      TOutputProperties assigned;
      assigned.getFileFormatProperties(kFormat);
      assigned = out;
      require(CountedGroup::s_alive == 4,
              "a copy does not own exactly one group per format");
      require(
          quality(assigned.getFileFormatProperties(kFormat))->getValue() == 42,
          "the copy lost the edited value");
    }
    require(CountedGroup::s_alive == 2,
            "deleted copies left their format properties behind");
  }
  require(CountedGroup::s_alive == 1,
          "deleted output properties left their format properties behind");
}

// A group read from a scene that another version saved, with a property the
// writer no longer has, a hidden property shown, or an enum saved without
// its range, gets the writer's layout and keeps its values.
void otherLayoutsAreReplaced() {
  TOutputProperties out;
  quality(out.getFileFormatProperties(kFormat))->setValue(42);

  out.getFileFormatProperties(kFormat)->add(
      new TIntProperty("Removed", 0, 10, 3));
  TPropertyGroup* group = out.getFileFormatProperties(kFormat);
  require(group->getPropertyCount() == 3 && !group->getProperty("Removed") &&
              quality(group)->getValue() == 42,
          "a group with an extra property was not given the writer's layout");
  require(CountedGroup::s_alive == 2, "the replaced group was not deleted");
  require(out.getFileFormatProperties(kFormat) == group,
          "a group in the writer's layout was replaced");

  group->getProperty("Palette")->setVisible(true);
  group = out.getFileFormatProperties(kFormat);
  require(!group->getProperty("Palette")->getVisible(),
          "a shown hidden property was not hidden again");

  mode(group)->deleteAllValues();
  mode(group)->addValue(L"Best");
  group = out.getFileFormatProperties(kFormat);
  require(
      mode(group)->getRange().size() == 2 && mode(group)->getValue() == L"Best",
      "an enum saved without its range did not get it back");
  require(CountedGroup::s_alive == 2, "a replaced group was not deleted");
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
    cleanupPaletteIsHeldOnce();
    defineWriter();
    formatPropertiesStay();
    otherLayoutsAreReplaced();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
  std::cout << "settingsownership: ok" << std::endl;
  return 0;
}
