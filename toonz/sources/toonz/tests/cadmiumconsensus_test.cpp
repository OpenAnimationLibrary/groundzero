#include "cadmiumconsensus.h"

#include <QJsonArray>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
const QColor red(230, 30, 20, 255), blue(20, 60, 230, 255);

void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

bool sameInts(const QVector<int> &actual, const QVector<int> &expected) {
  return actual.size() == expected.size() &&
         std::equal(actual.cbegin(), actual.cend(), expected.cbegin());
}

Cadmium::Frame reference(const QColor &color, int style) {
  Cadmium::Frame frame;
  frame.regionColors = {QColor(), color};
  frame.regionStyles = {-1, style};
  return frame;
}

Cadmium::Frame target(Cadmium::Kind kind = Cadmium::Kind::ToonzRaster) {
  Cadmium::Frame frame;
  frame.kind = kind;
  frame.regionColors.resize(3);
  return frame;
}

QJsonObject response(const QColor &color) {
  const QJsonArray null{-100, -100, -100, -100};
  return QJsonObject{{"target_color_ids", QJsonArray{-100, 1, -100}},
      {"target_colors_rgba", QJsonArray{null,
          QJsonArray{color.red(), color.green(), color.blue(), color.alpha()}, null}}};
}

QVector<Cadmium::RegionProposal> parsed(const Cadmium::Frame &ref,
                                       int index,
                                       const QJsonObject &prediction) {
  QVector<Cadmium::RegionProposal> result;
  QString error;
  require(Cadmium::parseReferenceProposals(prediction, ref, target(ref.kind),
                                           index, result, error),
          "a valid native reference response was rejected");
  return result;
}

void parsingAndAgreement() {
  const Cadmium::Frame ref = reference(red, 2);
  QVector<Cadmium::RegionProposal> first = parsed(ref, 0, response(red));
  require(first.size() == 3 && first[0].protectedRegion && first[0].selected == -2 &&
              first[0].candidates.isEmpty(),
          "background pixels became paint candidates");
  require(first[1].selected == 0 && first[1].candidates.size() == 1 &&
              first[1].candidates[0].style == 2 &&
              first[1].candidates[0].referenceRegion == 1 &&
              sameInts(first[1].candidates[0].supportingReferences, {0}),
          "a unique native reference fill was not selected with its provenance");
  require(first[2].selected == -1 && first[2].candidates.isEmpty() &&
              sameInts(first[2].references, {0}),
          "an unmatched region received an invented fill");
  QString error;
  require(Cadmium::mergeReferenceProposals(first, parsed(ref, 1, response(red)),
                                            ref.kind, error),
          "agreeing native references could not be merged");
  require(first[1].candidates.size() == 1 && first[1].selected == 0 &&
              sameInts(first[1].candidates[0].supportingReferences, {0, 1}) &&
              sameInts(first[0].references, {0, 1}),
          "agreement duplicated a fill or lost a reference contribution");
  require(!Cadmium::mergeReferenceProposals(first, parsed(ref, 1, response(red)),
                                             ref.kind, error) &&
              sameInts(first[1].references, {0, 1}),
          "a repeated reference was counted twice or partially changed a batch");

  Cadmium::Frame ambiguous = ref;
  ambiguous.regionColors.append(red);
  ambiguous.regionStyles.append(3);
  QVector<Cadmium::RegionProposal> intersection = parsed(ambiguous, 0, response(red));
  require(intersection[1].selected == -1 && intersection[1].candidates.size() == 2,
          "equal-colored native palette styles were silently collapsed");
  require(Cadmium::mergeReferenceProposals(intersection, parsed(ref, 1, response(red)),
                                            ref.kind, error) &&
              intersection[1].selected >= 0 &&
              intersection[1].candidates[intersection[1].selected].style == 2,
          "multiple references did not resolve a unique unanimous native style");
  QVector<Cadmium::RegionProposal> stillAmbiguous = parsed(ambiguous, 0, response(red));
  require(Cadmium::mergeReferenceProposals(stillAmbiguous,
              parsed(ambiguous, 1, response(red)), ref.kind, error) &&
              stillAmbiguous[1].selected == -1,
          "two unanimously supported fills were resolved arbitrarily");

  QVector<Cadmium::RegionProposal> conflict = parsed(ref, 0, response(red));
  require(Cadmium::mergeReferenceProposals(conflict,
              parsed(reference(blue, 3), 1, response(blue)), ref.kind, error) &&
              conflict[1].candidates.size() == 2 && conflict[1].selected == -1,
          "conflicting painted references were silently resolved");
  require(Cadmium::mergeReferenceProposals(conflict, parsed(ref, 2, response(red)),
                                            ref.kind, error) &&
              conflict[1].selected == -1,
          "a majority vote overrode a conflicting reference");

  QVector<Cadmium::RegionProposal> missing = parsed(ref, 0, response(red));
  QJsonObject noMatch = response(red);
  noMatch["target_color_ids"] = QJsonArray{-100, -100, -100};
  const QJsonArray null{-100, -100, -100, -100};
  noMatch["target_colors_rgba"] = QJsonArray{null, null, null};
  require(Cadmium::mergeReferenceProposals(missing, parsed(ref, 1, noMatch),
                                            ref.kind, error) &&
              missing[1].selected == -1,
          "an unmatched reference was misreported as unanimous agreement");
}

void retainedDecisions() {
  const Cadmium::Frame ref = reference(red, 2);
  QVector<Cadmium::RegionProposal> proposals = parsed(ref, 0, response(red));
  QString error;
  require(Cadmium::mergeReferenceProposals(proposals,
              parsed(reference(blue, 3), 1, response(blue)), ref.kind, error),
          "conflict fixture could not be created");
  require(Cadmium::chooseProposal(proposals[1], 1) && proposals[1].locked,
          "an artist decision was not locked");
  require(Cadmium::mergeReferenceProposals(proposals, parsed(ref, 2, response(red)),
                                            ref.kind, error) &&
              proposals[1].selected == 1 && proposals[1].candidates[1].color == blue,
          "a new model result replaced an artist-selected native fill");
  Cadmium::protectProposal(proposals[1], true);
  require(!Cadmium::chooseProposal(proposals[1], 0) &&
              Cadmium::mergeReferenceProposals(proposals, parsed(ref, 3, response(red)),
                                                ref.kind, error) &&
              proposals[1].protectedRegion && proposals[1].selected == 1,
          "a protected region was changed by a decision or model result");
  Cadmium::protectProposal(proposals[1], false);
  require(Cadmium::chooseProposal(proposals[1], -2) && proposals[1].locked &&
              Cadmium::mergeReferenceProposals(proposals, parsed(ref, 4, response(red)),
                                                ref.kind, error) &&
              proposals[1].selected == -2,
          "an explicit skip was replaced during continued generation");
  require(Cadmium::chooseProposal(proposals[1], -1) && !proposals[1].locked,
          "clearing a decision left the automatic choice locked");
  require(!Cadmium::chooseProposal(proposals[1], 99) &&
              !Cadmium::chooseProposal(proposals[0], 0),
          "an invalid or background decision was accepted");

  Cadmium::ProposalCandidate manual;
  manual.color = QColor(10, 210, 70);
  manual.style = 7;
  proposals[1].candidates.append(manual);
  require(Cadmium::chooseProposal(proposals[1], proposals[1].candidates.size() - 1) &&
              Cadmium::validateProposals(proposals, ref.kind, error),
          "an explicit artist-authored palette choice was rejected");
  require(Cadmium::proposalMemoryBytes(proposals) >=
              qint64(proposals.size()) * qint64(sizeof(Cadmium::RegionProposal)),
          "proposal memory accounting omitted its region storage");
}

void incompleteConsensus() {
  const Cadmium::Frame ref = reference(red, 2);
  QString error;
  const auto single = parsed(ref, 0, response(red));
  auto partial = single;
  Cadmium::invalidateIncompleteConsensus(partial, 2);
  require(partial[1].selected == -1 && partial[1].candidates.size() == 1 &&
              sameInts(partial[1].references, {0}),
          "an incomplete automatic consensus remained selected or lost its choices");

  auto complete = single;
  require(Cadmium::mergeReferenceProposals(complete, parsed(ref, 1, response(red)),
                                            ref.kind, error),
          "the complete consensus fixture could not be merged");
  Cadmium::invalidateIncompleteConsensus(complete, 2);
  require(complete[1].selected == 0,
          "a complete unanimous choice was invalidated");
  auto missingSupport = complete;
  missingSupport[1].candidates[0].supportingReferences = {0};
  Cadmium::invalidateIncompleteConsensus(missingSupport, 2);
  require(missingSupport[1].selected == -1,
          "processed references concealed a missing supporting prediction");
  auto wrongReferences = complete;
  wrongReferences[1].references = {1, 2};
  wrongReferences[1].candidates[0].supportingReferences = {1, 2};
  Cadmium::invalidateIncompleteConsensus(wrongReferences, 2);
  require(wrongReferences[1].selected == -1,
          "an equal-sized set of different references was treated as complete");
  auto duplicateReferences = complete;
  duplicateReferences[1].references = {0, 0};
  duplicateReferences[1].candidates[0].supportingReferences = {0, 0};
  Cadmium::invalidateIncompleteConsensus(duplicateReferences, 2);
  require(duplicateReferences[1].selected == -1,
          "duplicate references satisfied an incomplete consensus");

  auto locked = single;
  require(Cadmium::chooseProposal(locked[1], 0),
          "the retained artist choice fixture could not be selected");
  Cadmium::invalidateIncompleteConsensus(locked, 2);
  require(locked[1].selected == 0 && locked[1].locked,
          "an explicit locked fill was invalidated as an automatic choice");
  auto protectedRegion = single;
  Cadmium::protectProposal(protectedRegion[1], true);
  Cadmium::invalidateIncompleteConsensus(protectedRegion, 2);
  require(protectedRegion[1].selected == 0 && protectedRegion[1].protectedRegion,
          "a protected region decision was changed");
  auto skipped = single;
  require(Cadmium::chooseProposal(skipped[1], -2),
          "the retained skip fixture could not be selected");
  Cadmium::invalidateIncompleteConsensus(skipped, 2);
  require(skipped[1].selected == -2 && skipped[1].locked,
          "an explicit skip was invalidated");
  auto manual = single;
  manual[1].candidates[0].referenceIndex = -1;
  manual[1].candidates[0].referenceRegion = -1;
  manual[1].candidates[0].supportingReferences.clear();
  for (int count : {0, -1, Cadmium::MaximumProposalReferences + 1}) {
    auto automatic = complete;
    Cadmium::invalidateIncompleteConsensus(automatic, count);
    require(automatic[1].selected == -1,
            "an invalid reference count retained automatic acceptance");
    Cadmium::invalidateIncompleteConsensus(manual, count);
    require(manual[1].selected == 0,
            "an artist-authored fill was mistaken for an incomplete model choice");
  }
  auto invalidSelection = single;
  invalidSelection[1].selected = 99;
  Cadmium::invalidateIncompleteConsensus(invalidSelection, 2);
  require(invalidSelection[1].selected == -1,
          "a selected candidate outside its table was dereferenced or retained");
}

void exactNativeColors() {
  const Cadmium::NativeColor native{1.23456789, -0.1251234, 2.3456789, 0.456789,
                                     true, true};
  const QColor proxy(255, 0, 255, 116);
  Cadmium::Frame ref = reference(proxy, 0);
  ref.kind = Cadmium::Kind::FullColor;
  ref.nativeRegionColors = {{}, native};
  QVector<Cadmium::RegionProposal> exact = parsed(ref, 0, response(proxy));
  require(exact[1].selected == 0 &&
              exact[1].candidates[0].nativeColor == native,
          "native HDR, negative RGB or alpha precision was reduced");
  QString error;
  require(Cadmium::mergeReferenceProposals(exact, parsed(ref, 1, response(proxy)),
                                            ref.kind, error) &&
              exact[1].candidates.size() == 1 && exact[1].selected == 0,
          "identical native reference fills did not agree exactly");
  for (int variation = 0; variation < 3; ++variation) {
    Cadmium::Frame distinct = ref;
    if (variation == 0) distinct.nativeRegionColors[1].r += 0.00000001;
    if (variation == 1) distinct.nativeRegionColors[1].a += 0.00000001;
    if (variation == 2) distinct.nativeRegionColors[1].linear = false;
    QVector<Cadmium::RegionProposal> collision = parsed(ref, 0, response(proxy));
    require(Cadmium::mergeReferenceProposals(collision,
                parsed(distinct, 1, response(proxy)), ref.kind, error) &&
                collision[1].candidates.size() == 2 && collision[1].selected == -1,
            "different native fills sharing a display proxy were collapsed");
  }
  Cadmium::Frame nonflat = ref;
  nonflat.nativeRegionColors[1].valid = false;
  const auto unresolved = parsed(nonflat, 0, response(proxy));
  require(unresolved[1].candidates.isEmpty() && unresolved[1].selected == -1,
          "a display proxy stood in for a missing flat native reference fill");
  auto invalid = exact;
  invalid[1].candidates[0].nativeColor.r =
      std::numeric_limits<double>::infinity();
  require(!Cadmium::validateProposals(invalid, ref.kind, error),
          "a nonfinite persisted native reference fill was accepted");
}

void rejectsMalformedResponses() {
  const Cadmium::Frame ref = reference(red, 2);
  const QVector<Cadmium::RegionProposal> original = parsed(ref, 0, response(red));
  auto reject = [&](const QJsonObject &bad) {
    QVector<Cadmium::RegionProposal> unchanged = original;
    QString error;
    require(!Cadmium::parseReferenceProposals(bad, ref, target(), 1, unchanged, error) &&
                !error.isEmpty() && unchanged[1].selected == original[1].selected &&
                unchanged[1].candidates[0].style == 2 &&
                sameInts(unchanged[1].references, {0}),
            "a malformed response was accepted or partially changed a proposal");
  };
  reject(QJsonObject());
  QJsonObject bad = response(red);
  bad["target_color_ids"] = QJsonArray{-100, 1};
  reject(bad);
  for (const QJsonValue &id : {QJsonValue(1.5), QJsonValue(-99), QJsonValue(2),
                              QJsonValue(true), QJsonValue(QStringLiteral("1")),
                              QJsonValue(std::numeric_limits<double>::infinity())}) {
    bad = response(red);
    bad["target_color_ids"] = QJsonArray{-100, id, -100};
    reject(bad);
  }
  const QJsonArray null{-100, -100, -100, -100};
  for (const QJsonValue &pixel : {
          QJsonValue(QJsonArray{230, 30, 20}),
          QJsonValue(QJsonArray{230.5, 30, 20, 255}),
          QJsonValue(QJsonArray{-100, 30, 20, 255}),
          QJsonValue(QJsonArray{20, 60, 230, 255}),
          QJsonValue(QJsonArray{230, 30, 20, 256}),
          QJsonValue(null), QJsonValue(QStringLiteral("red"))}) {
    bad = response(red);
    bad["target_colors_rgba"] = QJsonArray{null, pixel, null};
    reject(bad);
  }
  bad = response(red);
  bad["target_color_ids"] = QJsonArray{-100, -100, -100};
  reject(bad);

  Cadmium::Frame wrong = ref;
  wrong.kind = Cadmium::Kind::Vector;
  QVector<Cadmium::RegionProposal> result;
  QString error;
  require(!Cadmium::parseReferenceProposals(response(red), wrong, target(), 0,
                                             result, error) &&
              !Cadmium::parseReferenceProposals(response(red), ref, target(),
                  Cadmium::MaximumProposalReferences, result, error),
          "an incompatible reference kind or invalid reference index was accepted");
  wrong = ref;
  wrong.regionStyles[1] = 65536;
  require(!Cadmium::parseReferenceProposals(response(red), wrong, target(), 0,
                                             result, error),
          "an invalid native palette style was accepted");
  wrong = ref;
  wrong.regionColors.clear(); wrong.regionStyles.clear();
  wrong.regionColors.append(QColor()); wrong.regionStyles.append(-1);
  for (int index = 0; index < Cadmium::MaximumRegionCandidates; ++index) {
    wrong.regionColors.append(QColor(index & 255, index >> 8, 0));
    wrong.regionStyles.append(index);
  }
  require(!Cadmium::parseReferenceProposals(response(red), wrong, target(), 0,
                                             result, error),
          "a reference exceeding the model palette limit was accepted");
}

void rejectsInvalidDecisionsAndBudgets() {
  QString error;
  const Cadmium::Frame ref = reference(red, 2);
  const auto original = parsed(ref, 0, response(red));
  auto reject = [&](QVector<Cadmium::RegionProposal> invalid) {
    require(!Cadmium::validateProposals(invalid, ref.kind, error) && !error.isEmpty(),
            "a corrupt persisted native decision was accepted");
  };
  auto invalid = original;
  invalid[0].protectedRegion = false;
  reject(invalid);
  invalid = original;
  invalid[1].selected = 99;
  reject(invalid);
  invalid = original;
  invalid[1].references.append(0);
  reject(invalid);
  invalid = original;
  invalid[1].candidates[0].supportingReferences = {1};
  reject(invalid);
  invalid = original;
  invalid[1].candidates[0].referenceIndex = -1;
  reject(invalid);
  invalid = original;
  invalid[1].candidates.append(invalid[1].candidates[0]);
  reject(invalid);

  QVector<Cadmium::ProposalCandidate> fills;
  for (int style = 0; style < Cadmium::MaximumRegionCandidates; ++style) {
    Cadmium::ProposalCandidate candidate = original[1].candidates[0];
    candidate.style = style;
    fills.append(candidate);
  }
  auto full = original;
  full[1].candidates = fills;
  full[1].selected = -1;
  require(Cadmium::validateProposals(full, ref.kind, error),
          "the supported native fill choice boundary was rejected");
  require(!Cadmium::mergeReferenceProposals(full,
              parsed(reference(red, Cadmium::MaximumRegionCandidates), 1, response(red)),
              ref.kind, error) && full[1].candidates.size() == Cadmium::MaximumRegionCandidates &&
              sameInts(full[0].references, {0}) && sameInts(full[1].references, {0}),
          "a fill choice overflow was accepted or partially committed");

  const int regionCount = Cadmium::MaximumProposalCandidates /
                          Cadmium::MaximumRegionCandidates + 2;
  QVector<Cadmium::RegionProposal> oversized(regionCount);
  oversized[0] = original[0];
  for (int region = 1; region < oversized.size(); ++region) {
    oversized[region].references = {0};
    oversized[region].candidates = fills;
  }
  require(!Cadmium::validateProposals(oversized, ref.kind, error),
          "the global native choice memory budget was not enforced");
}
}  // namespace

int main() {
  parsingAndAgreement();
  retainedDecisions();
  incompleteConsensus();
  exactNativeColors();
  rejectsMalformedResponses();
  rejectsInvalidDecisionsAndBudgets();
  std::cout << "Cadmium multi-reference consensus checks passed\n";
  return 0;
}
