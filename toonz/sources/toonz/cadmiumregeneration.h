#pragma once

#include "cadmiumconsensus.h"

#include <QHash>
#include <QSet>

namespace Cadmium {

enum class RegenerationMode {
  All,
  CurrentDrawing,
  UnresolvedRegions,
  SelectedRegions
};

struct RegenerationPlan {
  QVector<RegionProposal> regions;
  QSet<int> requested;
  QHash<int, int> previousToCurrent;
  bool invalidateReview = false;
  QString error;
  bool valid() const { return error.isEmpty(); }
};

// Selection contains labels from the previous preview. A selected mask that
// changed must be selected again from a new drawing proposal, never guessed.
// The caller chooses the drawing for CurrentDrawing and validates reference,
// palette, source identity, and extraction settings before retaining choices.
RegenerationPlan regenerationPlan(
    const Frame &previous, const Frame &current,
    const QVector<RegionProposal> &previousProposals,
    const QSet<int> &selectedPreviousRegions, RegenerationMode mode,
    bool applied = false);

}  // namespace Cadmium
