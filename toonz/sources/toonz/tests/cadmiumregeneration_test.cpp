#include "cadmiumregeneration.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

Cadmium::Frame drawing(Cadmium::Kind kind = Cadmium::Kind::ToonzRaster) {
  Cadmium::Frame frame;
  frame.kind = kind;
  frame.lines = QImage(8, 4, QImage::Format_ARGB32);
  frame.lines.fill(Qt::transparent);
  frame.labels.fill(0, 32);
  frame.regionColors.resize(3);
  for (int y : {1, 2}) {
    for (int x : {1, 2}) frame.labels[y * 8 + x] = 1;
    for (int x : {5, 6}) frame.labels[y * 8 + x] = 2;
  }
  frame.fingerprint = QByteArray("same drawing");
  if (kind == Cadmium::Kind::Vector)
    frame.nativeRegionIndices = {-1, 10, 20};
  return frame;
}

Cadmium::RegionProposal decision(int style, bool locked = false) {
  Cadmium::ProposalCandidate candidate;
  candidate.color = QColor(Qt::red);
  candidate.style = style;
  Cadmium::RegionProposal proposal;
  proposal.candidates.append(candidate);
  proposal.selected = 0;
  proposal.locked = locked;
  return proposal;
}

QVector<Cadmium::RegionProposal> decisions() {
  QVector<Cadmium::RegionProposal> proposals(3);
  proposals[0].selected = -2;
  proposals[0].locked = proposals[0].protectedRegion = true;
  proposals[1] = decision(1);
  proposals[2] = decision(2);
  return proposals;
}

void modesAndLocks() {
  using namespace Cadmium;
  const Frame frame = drawing();
  QVector<RegionProposal> previous = decisions();
  previous[1].locked = true;
  RegenerationPlan plan = regenerationPlan(
      frame, frame, previous, {}, RegenerationMode::All);
  require(plan.valid() && plan.requested.size() == 1 &&
              plan.requested.contains(2) && plan.invalidateReview,
          "full regeneration did not scope its unlocked regions");
  require(plan.regions[1].selected == 0 && plan.regions[1].locked &&
              plan.regions[1].candidates[0].style == 1 &&
              plan.regions[2].candidates.isEmpty() &&
              plan.regions[2].selected == -1,
          "full regeneration changed a locked fill or kept a replaced prediction");
  require(plan.regions[0].protectedRegion && plan.regions[0].locked &&
              plan.regions[0].selected == -2 && !plan.requested.contains(0),
          "background pixels were exposed to regeneration");

  previous[2].protectedRegion = true;
  plan = regenerationPlan(frame, frame, previous, {0, 1, 2},
                          RegenerationMode::SelectedRegions);
  require(plan.valid() && plan.requested.isEmpty() && !plan.invalidateReview &&
              plan.regions[2].protectedRegion && plan.regions[2].selected == 0,
          "selected regeneration replaced a locked or protected decision");

  previous = decisions();
  previous[2] = RegionProposal();
  plan = regenerationPlan(frame, frame, previous, {},
                          RegenerationMode::UnresolvedRegions);
  require(plan.valid() && plan.requested.size() == 1 &&
              plan.requested.contains(2) && plan.regions[1].selected == 0,
          "unresolved regeneration did not retain an accepted decision");
  previous[2].selected = -2;
  plan = regenerationPlan(frame, frame, previous, {},
                          RegenerationMode::UnresolvedRegions);
  require(plan.valid() && plan.requested.isEmpty() &&
              plan.regions[2].selected == -2,
          "an explicit skip was treated as an unresolved prediction");
  plan = regenerationPlan(frame, frame, decisions(), {},
                          RegenerationMode::CurrentDrawing);
  require(plan.valid() && plan.requested.size() == 2,
          "drawing regeneration did not request its eligible regions");
}

void exactMasks() {
  using namespace Cadmium;
  const Frame original = drawing();
  Frame renumbered = original;
  for (int &label : renumbered.labels)
    if (label > 0) label = 3 - label;
  QVector<RegionProposal> previous = decisions();
  RegenerationPlan plan = regenerationPlan(
      original, renumbered, previous, {1}, RegenerationMode::SelectedRegions);
  require(plan.valid() && plan.previousToCurrent.value(1, -1) == 2 &&
              plan.previousToCurrent.value(2, -1) == 1 &&
              plan.requested.size() == 1 && plan.requested.contains(2) &&
              plan.regions[1].candidates[0].style == 2 &&
              plan.regions[2].candidates.isEmpty(),
          "renumbered labels redirected a decision or regenerated the wrong mask");

  previous[1].locked = true;
  Frame changed = original;
  changed.labels[1 * 8 + 5] = 0;
  plan = regenerationPlan(original, changed, previous, {}, RegenerationMode::All);
  require(plan.valid() && plan.previousToCurrent.value(1, -1) == 1 &&
              !plan.previousToCurrent.contains(2) &&
              plan.regions[1].locked && plan.regions[1].selected == 0 &&
              plan.requested.size() == 1 && plan.requested.contains(2),
          "an unrelated region edit discarded an unchanged manual fill");

  changed = original;
  changed.labels[1 * 8 + 1] = 0;
  changed.labels[1 * 8] = 1;
  plan = regenerationPlan(original, changed, previous, {}, RegenerationMode::All);
  require(plan.valid() && !plan.previousToCurrent.contains(1) &&
              !plan.regions[1].locked && plan.regions[1].candidates.isEmpty() &&
              plan.requested.contains(1),
          "a moved mask inherited a locked decision from its old label number");
  plan = regenerationPlan(original, changed, previous, {1},
                          RegenerationMode::SelectedRegions);
  require(!plan.valid() && !plan.error.isEmpty() && plan.requested.isEmpty() &&
              plan.regions.isEmpty() && previous[1].locked &&
              previous[1].candidates[0].style == 1,
          "changed selected geometry was guessed or the existing proposal was mutated");

  changed = original;
  changed.regionColors.resize(4);
  changed.labels[1 * 8 + 1] = 3;
  plan = regenerationPlan(original, changed, previous, {}, RegenerationMode::All);
  require(plan.valid() && !plan.previousToCurrent.contains(1) &&
              plan.requested.contains(1) && plan.requested.contains(3) &&
              !plan.regions[1].locked && !plan.regions[3].locked,
          "a split region inherited a stale manual decision");
  changed = original;
  for (int &label : changed.labels) if (label == 2) label = 1;
  plan = regenerationPlan(original, changed, previous, {}, RegenerationMode::All);
  require(plan.valid() && !plan.previousToCurrent.contains(1) &&
              !plan.previousToCurrent.contains(2) &&
              plan.requested.size() == 1 && plan.requested.contains(1) &&
              plan.regions[2].protectedRegion,
          "merged or absent masks inherited stale decisions");

  changed = original;
  changed.regionColors.resize(4);
  changed.labels[0] = 3;
  previous[2].locked = true;
  plan = regenerationPlan(original, changed, previous, {}, RegenerationMode::All);
  require(plan.valid() && plan.regions[1].locked && plan.regions[2].locked &&
              plan.requested.size() == 1 && plan.requested.contains(3),
          "a new region invalidated unrelated exact manual masks");

  changed = original;
  changed.fingerprint = QByteArray("edited samples");
  plan = regenerationPlan(original, changed, previous, {},
                          RegenerationMode::UnresolvedRegions);
  require(plan.valid() && plan.requested.isEmpty() && plan.invalidateReview,
          "changed source samples retained a stale reviewed state");
}

void nativeVectorIdentity() {
  using namespace Cadmium;
  const Frame original = drawing(Kind::Vector);
  QVector<RegionProposal> previous = decisions();
  previous[1].locked = true;
  RegenerationPlan plan = regenerationPlan(
      original, original, previous, {}, RegenerationMode::All);
  require(plan.valid() && plan.regions[1].locked && !plan.requested.contains(1),
          "an unchanged native vector region lost its explicit decision");
  Frame changed = original;
  changed.fingerprint = QByteArray("subpixel stroke edit");
  plan = regenerationPlan(original, changed, previous, {}, RegenerationMode::All);
  require(plan.valid() && !plan.regions[1].locked && plan.requested.contains(1),
          "an unchanged analysis proxy concealed a native vector geometry edit");
  changed = original;
  changed.nativeRegionIndices[1] = 99;
  plan = regenerationPlan(original, changed, previous, {1},
                          RegenerationMode::SelectedRegions);
  require(!plan.valid() && plan.requested.isEmpty(),
          "a different native vector region inherited the old selected mask");
  changed = original;
  changed.nativeRegionIndices[1] = -1;
  plan = regenerationPlan(original, changed, previous, {}, RegenerationMode::All);
  require(plan.valid() && plan.regions[1].protectedRegion &&
              !plan.requested.contains(1),
          "an unavailable native vector region was sent for regeneration");
}

void limitsAndApplication() {
  using namespace Cadmium;
  const Frame original = drawing();
  const QVector<RegionProposal> previous = decisions();
  RegenerationPlan plan = regenerationPlan(
      original, original, previous, {}, RegenerationMode::All, true);
  require(!plan.valid() && plan.regions.isEmpty() && plan.requested.isEmpty() &&
              previous[1].selected == 0,
          "an applied drawing was regenerated or mutated");
  plan = regenerationPlan(Frame(), original, {}, {}, RegenerationMode::All);
  require(plan.valid() && plan.requested.size() == 2,
          "a new drawing could not generate its first proposals");
  plan = regenerationPlan(Frame(), original, {}, {1},
                          RegenerationMode::SelectedRegions);
  require(!plan.valid(), "a selection without a previous mask was guessed");
  Frame invalid = original;
  invalid.labels[0] = -1;
  require(!regenerationPlan(original, invalid, previous, {},
                            RegenerationMode::All).valid(),
          "a negative region label was accepted");
  invalid = original;
  invalid.labels[0] = invalid.regionColors.size();
  require(!regenerationPlan(original, invalid, previous, {},
                            RegenerationMode::All).valid(),
          "an out-of-range region label was accepted");
  invalid = original;
  invalid.labels.removeLast();
  require(!regenerationPlan(original, invalid, previous, {},
                            RegenerationMode::All).valid(),
          "a truncated analysis mask was accepted");
  invalid = original;
  invalid.kind = Kind(99);
  require(!regenerationPlan(original, invalid, previous, {},
                            RegenerationMode::All).valid(),
          "an unknown drawing kind was accepted");
  invalid = original;
  invalid.regionColors.resize(MaximumProposalRegions + 1);
  require(!regenerationPlan(original, invalid, previous, {},
                            RegenerationMode::All).valid(),
          "an excessive region count was accepted");
  QVector<RegionProposal> malformed = previous;
  malformed[1].selected = 17;
  require(!regenerationPlan(original, original, malformed, {},
                            RegenerationMode::All).valid(),
          "an invalid saved decision was accepted");

  Frame large;
  large.lines = QImage(4, 1, QImage::Format_ARGB32);
  large.lines.fill(Qt::transparent);
  large.regionColors.resize(MaximumProposalRegions);
  large.labels = {0, 65536, 0, 0};
  QVector<RegionProposal> many(MaximumProposalRegions);
  many[0].selected = -2;
  many[0].locked = many[0].protectedRegion = true;
  many[65536] = decision(1, true);
  Frame renumbered = large;
  renumbered.labels[1] = 65535;
  plan = regenerationPlan(large, renumbered, many, {}, RegenerationMode::All);
  require(plan.valid() && plan.previousToCurrent.value(65536, -1) == 65535 &&
              plan.regions[65535].locked &&
              plan.regions[65535].candidates[0].style == 1 &&
              plan.requested.isEmpty(),
          "high region ids wrapped or lost exact mask identity");
}
}

int main() {
  modesAndLocks();
  exactMasks();
  nativeVectorIdentity();
  limitsAndApplication();
  std::cout << "Cadmium selective regeneration checks passed\n";
  return 0;
}
