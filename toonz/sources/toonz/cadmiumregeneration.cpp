#include "cadmiumregeneration.h"

#include <algorithm>

namespace Cadmium {
namespace {
constexpr qint64 MaximumRegenerationPixels = 16LL * 1024 * 1024;

bool inspect(const Frame &frame, QVector<bool> &observed, QString &error) {
  const qint64 pixels = qint64(frame.lines.width()) * frame.lines.height();
  const int regions = frame.regionColors.size();
  if (frame.lines.isNull() || pixels <= 0 ||
      pixels > MaximumRegenerationPixels || pixels != frame.labels.size() ||
      regions <= 0 || regions > MaximumProposalRegions ||
      (frame.kind != Kind::ToonzRaster && frame.kind != Kind::Vector &&
       frame.kind != Kind::FullColor) ||
      (frame.kind == Kind::Vector &&
       frame.nativeRegionIndices.size() != regions)) {
    error = QStringLiteral("The drawing analysis is unavailable or exceeds regeneration limits.");
    return false;
  }
  observed.fill(false, regions);
  for (int label : frame.labels) {
    if (label < 0 || label >= regions) {
      error = QStringLiteral("The drawing analysis contains an invalid region label.");
      return false;
    }
    observed[label] = true;
  }
  return true;
}

void bindLabel(QVector<int> &mapping, int from, int to) {
  if (from <= 0) return;
  if (to <= 0 || (mapping[from] != -1 && mapping[from] != to))
    mapping[from] = -2;
  else if (mapping[from] == -1)
    mapping[from] = to;
}

bool hasState(const RegionProposal &proposal) {
  return proposal.selected != -1 || proposal.locked ||
         proposal.protectedRegion || !proposal.candidates.isEmpty() ||
         !proposal.references.isEmpty();
}

void protectUnpaintable(RegionProposal &proposal) {
  proposal = RegionProposal();
  proposal.selected = -2;
  proposal.locked = true;
  proposal.protectedRegion = true;
}
}

RegenerationPlan regenerationPlan(
    const Frame &previous, const Frame &current,
    const QVector<RegionProposal> &previousProposals,
    const QSet<int> &selectedPreviousRegions, RegenerationMode mode,
    bool applied) {
  RegenerationPlan plan;
  if (applied) {
    plan.error = QStringLiteral("Applied drawings cannot be regenerated. Undo the application or choose another drawing.");
    return plan;
  }
  if (mode != RegenerationMode::All &&
      mode != RegenerationMode::CurrentDrawing &&
      mode != RegenerationMode::UnresolvedRegions &&
      mode != RegenerationMode::SelectedRegions) {
    plan.error = QStringLiteral("The regeneration mode is invalid.");
    return plan;
  }
  QVector<bool> currentObserved;
  if (!inspect(current, currentObserved, plan.error)) return plan;
  plan.regions.resize(current.regionColors.size());
  for (int region = 0; region < plan.regions.size(); ++region)
    if (region == 0 || !currentObserved[region] ||
        (current.kind == Kind::Vector && current.nativeRegionIndices[region] < 0))
      protectUnpaintable(plan.regions[region]);

  const bool hasPrevious = !previous.lines.isNull() ||
                           !previous.labels.isEmpty() ||
                           !previousProposals.isEmpty();
  QVector<bool> previousObserved;
  if (hasPrevious) {
    if (!inspect(previous, previousObserved, plan.error)) return plan;
    if ((!previousProposals.isEmpty() &&
         previousProposals.size() != previous.regionColors.size()) ||
        (!previousProposals.isEmpty() &&
         !validateProposals(previousProposals, previous.kind, plan.error))) {
      if (plan.error.isEmpty())
        plan.error = QStringLiteral("The saved region decisions do not match the drawing analysis.");
      return plan;
    }
    const bool compatible = previous.kind == current.kind &&
        previous.lines.size() == current.lines.size() &&
        (current.kind != Kind::Vector ||
         (!previous.fingerprint.isEmpty() &&
          previous.fingerprint == current.fingerprint));
    if (compatible) {
      QVector<int> forward(previous.regionColors.size(), -1);
      QVector<int> backward(current.regionColors.size(), -1);
      // Mutual single-label mappings prove exact equality of both masks in
      // one scan. Background overlap, splits, merges, and added pixels break
      // the bijection; region numbers themselves never establish identity.
      for (int pixel = 0; pixel < previous.labels.size(); ++pixel) {
        bindLabel(forward, previous.labels[pixel], current.labels[pixel]);
        bindLabel(backward, current.labels[pixel], previous.labels[pixel]);
      }
      plan.previousToCurrent.insert(0, 0);
      for (int oldRegion = 1; oldRegion < forward.size(); ++oldRegion) {
        const int region = forward[oldRegion];
        if (region <= 0 || backward[region] != oldRegion) continue;
        if (current.kind == Kind::Vector &&
            current.nativeRegionIndices[region] < 0) continue;
        if (current.kind == Kind::Vector &&
            previous.nativeRegionIndices[oldRegion] !=
                current.nativeRegionIndices[region]) continue;
        plan.previousToCurrent.insert(oldRegion, region);
        if (!previousProposals.isEmpty())
          plan.regions[region] = previousProposals[oldRegion];
      }
    }
    for (int region = 1; region < previousObserved.size(); ++region)
      if (previousObserved[region] &&
          !plan.previousToCurrent.contains(region))
        plan.invalidateReview = true;
    if (previous.kind != current.kind ||
        previous.lines.size() != current.lines.size() ||
        previous.fingerprint != current.fingerprint)
      plan.invalidateReview = true;
    for (int region = 1; region < previousProposals.size(); ++region)
      if (hasState(previousProposals[region]) &&
          !plan.previousToCurrent.contains(region))
        plan.invalidateReview = true;
  }

  QSet<int> selected;
  if (mode == RegenerationMode::SelectedRegions) {
    for (int oldRegion : selectedPreviousRegions) {
      if (oldRegion == 0) continue;
      if (!hasPrevious || oldRegion < 0 ||
          oldRegion >= previous.regionColors.size() ||
          !plan.previousToCurrent.contains(oldRegion)) {
        plan.error = QStringLiteral("A selected region changed or disappeared. Generate a new drawing proposal and select its current regions.");
        plan.regions.clear();
        plan.requested.clear();
        plan.previousToCurrent.clear();
        return plan;
      }
      selected.insert(plan.previousToCurrent.value(oldRegion));
    }
  }
  for (int region = 1; region < plan.regions.size(); ++region) {
    const RegionProposal &proposal = plan.regions[region];
    if (!currentObserved[region] || proposal.locked ||
        proposal.protectedRegion) continue;
    if (mode == RegenerationMode::SelectedRegions &&
        !selected.contains(region)) continue;
    if (mode == RegenerationMode::UnresolvedRegions &&
        proposal.selected != -1) continue;
    plan.requested.insert(region);
    plan.regions[region] = RegionProposal();
  }
  if (!plan.requested.isEmpty()) plan.invalidateReview = true;
  return plan;
}

}  // namespace Cadmium
