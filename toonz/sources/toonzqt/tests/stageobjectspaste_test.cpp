// Pasting columns whose effect is linked to an effect outside the copied
// selection. Linked parameters are shared objects whose expression grammar is
// owned by the stage object tree of one xsheet. The copy keeps the link only in
// the xsheet of the linked effect; pasted elsewhere, for example in another
// scene, it gets its own parameters and the destination grammar, so closing
// the source scene cannot leave it evaluating through a deleted grammar.

#include "toonzqt/stageobjectsdata.h"

#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tfx.h"
#include "tparamcontainer.h"

#include <QCoreApplication>

#include <iostream>
#include <list>
#include <set>

namespace {
int failures = 0;

void check(bool condition, const char *message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

TDoubleParam *valueParam(TFx *fx) {
  return dynamic_cast<TDoubleParam *>(fx->getParams()->getParam("value"));
}

TFx *addInternalFx(TXsheet *xsh, TFx *input) {
  TFx *fx = TFx::create("addFx");
  if (!fx) return nullptr;
  xsh->getFxDag()->assignUniqueId(fx);
  xsh->getFxDag()->getInternalFxs()->addFx(fx);
  fx->getInputPort(0)->setFx(input);
  return fx;
}

// The pasted fx of the destination: an internal fx that is not one of the
// fixture effects.
TFx *pastedFx(TXsheet *xsh, const std::set<TFx *> &existing) {
  TFxSet *internal = xsh->getFxDag()->getInternalFxs();
  for (int i = 0; i < internal->getFxCount(); ++i)
    if (!existing.count(internal->getFx(i))) return internal->getFx(i);
  return nullptr;
}

void paste(const StageObjectsData &data, TXsheet *xsh, int column) {
  std::set<int> columns{column};
  std::list<int> splines;
  QMap<TStageObjectId, TStageObjectId> ids;
  QMap<TFx *, TFx *> fxs;
  data.restoreObjects(columns, splines, xsh, StageObjectsData::eDoClone, ids,
                      fxs);
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);

  TXsheetP source      = new TXsheet;
  TXsheetP destination = new TXsheet;
  source->insertColumn(0);
  source->insertColumn(1);
  destination->insertColumn(0);

  // "selected" is downstream of column 0 and is copied with it; "external"
  // is downstream of column 1, outside the selection, and shares its
  // parameters with "selected".
  TFxP selected =
      addInternalFx(source.getPointer(), source->getColumn(0)->getFx());
  TFxP external =
      addInternalFx(source.getPointer(), source->getColumn(1)->getFx());
  if (!selected || !external || !valueParam(external.getPointer())) {
    std::cerr << "FAIL: the add effect is not available\n";
    return 1;
  }
  TDoubleParam *shared = valueParam(external.getPointer());
  shared->setGrammar(source->getStageObjectTree()->getGrammar());
  TDoubleKeyframe start(0), end(10);
  start.m_type = end.m_type = TDoubleKeyframe::Expression;
  start.m_expressionText = end.m_expressionText = "frame*3";
  shared->setKeyframe(start);
  shared->setKeyframe(end);
  selected->linkParams(external.getPointer());
  const double expected = shared->getValue(4);
  check(expected > 0, "the fixture expression was not evaluated");

  // The clipboard copy of column 0, as the Copy command stores it.
  auto *clipboard = new StageObjectsData;
  const int flags =
      StageObjectsData::eDoClone | StageObjectsData::eResetFxDagPositions;
  clipboard->storeColumns({0}, source.getPointer(), flags);
  clipboard->storeColumnFxs({0}, source.getPointer(), flags);

  // Pasted in its own xsheet, the copy stays linked to the external effect.
  paste(*clipboard, source.getPointer(), 2);
  TFx *local = pastedFx(source.getPointer(),
                        {selected.getPointer(), external.getPointer()});
  check(local && valueParam(local) == shared,
        "a paste in the source xsheet detached the external parameter link");

  // Pasted in another xsheet, it keeps its own parameters and expressions, in
  // the context of the destination.
  paste(*clipboard, destination.getPointer(), 1);
  TFx *foreign               = pastedFx(destination.getPointer(), {});
  TDoubleParam *foreignValue = foreign ? valueParam(foreign) : nullptr;
  check(foreignValue && foreignValue != shared,
        "a paste in another xsheet shares the parameters of its source");
  check(foreign && foreign->getLinkedFx() == foreign,
        "a paste in another xsheet is linked to an effect of its source");
  check(foreignValue && foreignValue->getGrammar() ==
                            destination->getStageObjectTree()->getGrammar(),
        "a paste in another xsheet kept the expression grammar of its source");
  check(foreignValue && foreignValue->getKeyframeCount() == 2 &&
            foreignValue->getKeyframe(0).m_expressionText == "frame*3",
        "a paste in another xsheet lost its expression");
  check(shared->getGrammar() == source->getStageObjectTree()->getGrammar(),
        "a paste in another xsheet changed the grammar of the source effect");

  // Close the source: its stage object tree, and grammar, are deleted. The
  // pasted expression is evaluated and parsed again in its own context.
  delete clipboard;
  local    = nullptr;
  selected = TFxP();
  external = TFxP();
  shared   = nullptr;
  source   = TXsheetP();
  if (foreignValue) {
    check(foreignValue->getValue(4) == expected,
          "the pasted expression changed value after its source was closed");
    TDoubleKeyframe edited  = foreignValue->getKeyframe(0);
    edited.m_expressionText = "frame*2";
    foreignValue->setKeyframe(0, edited);
    check(foreignValue->getValue(4) == expected * 2 / 3,
          "the pasted expression could not be edited after its source was "
          "closed");
  }

  if (failures) return 1;
  std::cout << "stage object paste: all checks passed\n";
  return 0;
}
