#include "cadmiumconsensus.h"

#include "cadmiumcolor.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace Cadmium {
namespace {
using CandidateKey = std::tuple<int, QRgb, double, double, double, double, bool>;

bool supportedKind(Kind kind) {
  return kind == Kind::ToonzRaster || kind == Kind::Vector ||
         kind == Kind::FullColor;
}

bool validNative(const NativeColor &color) {
  return color.valid && std::isfinite(color.r) && std::isfinite(color.g) &&
         std::isfinite(color.b) && std::isfinite(color.a) &&
         color.a >= 0.0 && color.a <= 1.0;
}

CandidateKey keyOf(const ProposalCandidate &candidate, Kind kind) {
  if (kind == Kind::FullColor)
    return CandidateKey(0, 0, candidate.nativeColor.r, candidate.nativeColor.g,
                        candidate.nativeColor.b, candidate.nativeColor.a,
                        candidate.nativeColor.linear);
  return CandidateKey(candidate.style, candidate.color.rgba(), 0, 0, 0, 0, false);
}

bool indices(const QVector<int> &values, QSet<int> &seen) {
  if (values.size() > MaximumProposalReferences) return false;
  for (int value : values) {
    if (value < 0 || value >= MaximumProposalReferences || seen.contains(value))
      return false;
    seen.insert(value);
  }
  return true;
}

bool integer(const QJsonValue &value, int low, int high, int &result) {
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) || number != std::floor(number) ||
      number < low || number > high) return false;
  result = static_cast<int>(number);
  return true;
}

void selectUnanimous(RegionProposal &region) {
  if (region.locked || region.protectedRegion) return;
  region.selected = -1;
  if (region.references.isEmpty()) return;
  for (int candidate = 0; candidate < region.candidates.size(); ++candidate) {
    const ProposalCandidate &fill = region.candidates[candidate];
    if (fill.supportingReferences.size() != region.references.size()) continue;
    if (region.selected >= 0) {
      region.selected = -1;
      return;
    }
    region.selected = candidate;
  }
}
}  // namespace

bool sameCandidate(const ProposalCandidate &left,
                   const ProposalCandidate &right, Kind kind) {
  if (!supportedKind(kind)) return false;
  if (kind == Kind::FullColor)
    return validNative(left.nativeColor) && validNative(right.nativeColor) &&
           left.nativeColor == right.nativeColor;
  return left.style == right.style && left.color.isValid() &&
         right.color.isValid() && left.color.rgba() == right.color.rgba();
}

bool validateProposals(const QVector<RegionProposal> &proposals, Kind kind,
                       QString &error) {
  error.clear();
  if (!supportedKind(kind) || proposals.size() > MaximumProposalRegions) {
    error = QStringLiteral("The proposal format or region count is unsupported.");
    return false;
  }
  qint64 total = 0;
  for (int index = 0; index < proposals.size(); ++index) {
    const RegionProposal &region = proposals[index];
    if (region.candidates.size() > MaximumRegionCandidates ||
        region.selected < -2 || region.selected >= region.candidates.size() ||
        (index == 0 && (!region.protectedRegion || region.selected != -2 ||
                        !region.candidates.isEmpty()))) {
      error = QStringLiteral("The proposal contains an invalid region decision.");
      return false;
    }
    total += region.candidates.size();
    if (total > MaximumProposalCandidates) {
      error = QStringLiteral("The proposal exceeds its native fill choice budget.");
      return false;
    }
    QSet<int> references;
    if (!indices(region.references, references)) {
      error = QStringLiteral("The proposal contains invalid reference contributions.");
      return false;
    }
    std::set<CandidateKey> seen;
    for (const ProposalCandidate &candidate : region.candidates) {
      const bool manual = candidate.referenceIndex == -1 &&
                          candidate.referenceRegion == -1;
      if (!candidate.color.isValid() ||
          (!manual && (candidate.referenceIndex < 0 ||
                       candidate.referenceIndex >= MaximumProposalReferences ||
                       candidate.referenceRegion < 0 ||
                       candidate.referenceRegion >= MaximumProposalRegions)) ||
          (kind == Kind::FullColor
               ? candidate.style != 0 || !validNative(candidate.nativeColor)
               : candidate.style < 0 || candidate.style > 65535) ||
          !seen.insert(keyOf(candidate, kind)).second) {
        error = QStringLiteral("The proposal contains an invalid or repeated native fill.");
        return false;
      }
      QSet<int> supporters;
      if (!indices(candidate.supportingReferences, supporters) ||
          (!manual && !supporters.contains(candidate.referenceIndex))) {
        error = QStringLiteral("The proposal contains invalid fill provenance.");
        return false;
      }
      for (int reference : supporters)
        if (!references.contains(reference)) {
          error = QStringLiteral("A fill refers to an unprocessed reference.");
          return false;
        }
    }
  }
  return true;
}

bool parseReferenceProposals(const QJsonObject &response,
                             const Frame &reference, const Frame &target,
                             int referenceIndex,
                             QVector<RegionProposal> &proposals,
                             QString &error) {
  error.clear();
  const int regionCount = target.regionColors.size();
  if (!supportedKind(target.kind) || reference.kind != target.kind ||
      regionCount <= 0 || regionCount > MaximumProposalRegions ||
      reference.regionColors.isEmpty() ||
      reference.regionColors.size() > MaximumProposalRegions ||
      referenceIndex < 0 || referenceIndex >= MaximumProposalReferences ||
      (target.kind != Kind::FullColor &&
       reference.regionStyles.size() != reference.regionColors.size()) ||
      (target.kind == Kind::FullColor &&
       reference.nativeRegionColors.size() != reference.regionColors.size())) {
    error = QStringLiteral("The target or painted reference has an invalid native region table.");
    return false;
  }
  if (!response.value(QStringLiteral("target_color_ids")).isArray() ||
      !response.value(QStringLiteral("target_colors_rgba")).isArray()) {
    error = QStringLiteral("The worker omitted region predictions.");
    return false;
  }
  const QJsonArray ids = response.value(QStringLiteral("target_color_ids")).toArray();
  const QJsonArray colors = response.value(QStringLiteral("target_colors_rgba")).toArray();
  if (ids.size() != regionCount || colors.size() != regionCount) {
    error = QStringLiteral("The worker returned the wrong number of region predictions.");
    return false;
  }
  QVector<QColor> palette;
  for (const QColor &color : reference.regionColors)
    if (!palette.contains(color)) palette.append(color);
  if (palette.size() > MaximumRegionCandidates) {
    error = QStringLiteral("The reference exceeds the model palette limit.");
    return false;
  }

  QVector<RegionProposal> parsed(regionCount);
  parsed[0].protectedRegion = true;
  parsed[0].locked = true;
  parsed[0].selected = -2;
  QHash<QRgb, QVector<ProposalCandidate>> choices;
  qint64 candidateCount = 0;
  for (int region = 0; region < regionCount; ++region) {
    parsed[region].references.append(referenceIndex);
    int id;
    if (!integer(ids[region], -100, palette.size() - 1, id) ||
        (id < 0 && id != -100) || !colors[region].isArray()) {
      error = QStringLiteral("The worker returned an invalid palette identifier.");
      return false;
    }
    const QJsonArray rgba = colors[region].toArray();
    int channels[4] = {};
    if (rgba.size() != 4) {
      error = QStringLiteral("The worker returned an invalid region color tuple.");
      return false;
    }
    bool null = true, color = true;
    for (int channel = 0; channel < 4; ++channel) {
      if (!integer(rgba[channel], -100, 255, channels[channel])) {
        error = QStringLiteral("The worker returned a malformed color channel.");
        return false;
      }
      null &= channels[channel] == -100;
      color &= channels[channel] >= 0;
    }
    if (null) {
      if (id != -100 && palette[id].isValid()) {
        error = QStringLiteral("The worker returned a null color for a painted palette entry.");
        return false;
      }
      continue;
    }
    if (!color || id == -100 || !palette[id].isValid()) {
      error = QStringLiteral("The worker palette identifier and predicted color disagree.");
      return false;
    }
    const QColor proxy(channels[0], channels[1], channels[2], channels[3]);
    if (proxy != palette[id]) {
      error = QStringLiteral("The worker palette identifier and predicted color disagree.");
      return false;
    }
    if (region == 0) continue;
    if (!choices.contains(proxy.rgba())) {
      QVector<ProposalCandidate> available;
      QVector<int> sourceRegions;
      if (target.kind == Kind::FullColor) {
        sourceRegions = matchingNativeRegions(reference, proxy);
      } else {
        QSet<int> styles;
        for (int source = 0; source < reference.regionColors.size(); ++source) {
          if (reference.regionColors[source] != proxy) continue;
          const int style = reference.regionStyles[source];
          if (style < 0) continue;
          if (style > 65535 ||
              (target.palette && style >= target.palette->getStyleCount()) ||
              (reference.palette && style >= reference.palette->getStyleCount())) {
            error = QStringLiteral("The reference contains an invalid native palette style.");
            return false;
          }
          if (!styles.contains(style)) {
            styles.insert(style);
            sourceRegions.append(source);
          }
        }
      }
      if (sourceRegions.size() > MaximumRegionCandidates) {
        error = QStringLiteral("Too many exact native fills share one model color. Choose fewer references.");
        return false;
      }
      for (int source : sourceRegions) {
        ProposalCandidate candidate;
        candidate.color = proxy;
        candidate.style = target.kind == Kind::FullColor ? 0 : reference.regionStyles[source];
        if (target.kind == Kind::FullColor)
          candidate.nativeColor = reference.nativeRegionColors[source];
        candidate.referenceIndex = referenceIndex;
        candidate.referenceRegion = source;
        candidate.supportingReferences.append(referenceIndex);
        available.append(candidate);
      }
      choices.insert(proxy.rgba(), available);
    }
    parsed[region].candidates = choices.value(proxy.rgba());
    candidateCount += parsed[region].candidates.size();
    if (candidateCount > MaximumProposalCandidates) {
      error = QStringLiteral("The proposal exceeds its native fill choice budget.");
      return false;
    }
    selectUnanimous(parsed[region]);
  }
  if (!validateProposals(parsed, target.kind, error)) return false;
  proposals.swap(parsed);
  return true;
}

bool mergeReferenceProposals(QVector<RegionProposal> &proposals,
                             const QVector<RegionProposal> &referenceProposals,
                             Kind kind, QString &error) {
  error.clear();
  if (!validateProposals(proposals, kind, error) ||
      !validateProposals(referenceProposals, kind, error)) return false;
  if (referenceProposals.isEmpty() ||
      (!proposals.isEmpty() && proposals.size() != referenceProposals.size())) {
    error = QStringLiteral("The reference predictions do not match the target regions.");
    return false;
  }
  if (proposals.isEmpty()) {
    proposals = referenceProposals;
    return true;
  }
  QVector<RegionProposal> merged = proposals;
  for (int index = 0; index < merged.size(); ++index) {
    RegionProposal &region = merged[index];
    const RegionProposal &incoming = referenceProposals[index];
    if (region.locked || region.protectedRegion) {
      for (int reference : incoming.references)
        if (!region.references.contains(reference)) region.references.append(reference);
      continue;
    }
    for (int reference : incoming.references) {
      if (region.references.contains(reference)) {
        error = QStringLiteral("A reference contributed predictions more than once.");
        return false;
      }
      region.references.append(reference);
    }
    if (region.references.size() > MaximumProposalReferences) {
      error = QStringLiteral("Too many references were assigned to one drawing.");
      return false;
    }
    for (const ProposalCandidate &candidate : incoming.candidates) {
      auto found = std::find_if(region.candidates.begin(), region.candidates.end(),
          [&](const ProposalCandidate &current) { return sameCandidate(current, candidate, kind); });
      if (found == region.candidates.end()) {
        if (region.candidates.size() == MaximumRegionCandidates) {
          error = QStringLiteral("The region exceeds its native fill choice limit.");
          return false;
        }
        region.candidates.append(candidate);
      } else {
        for (int reference : candidate.supportingReferences)
          if (!found->supportingReferences.contains(reference))
            found->supportingReferences.append(reference);
      }
    }
    selectUnanimous(region);
  }
  if (!validateProposals(merged, kind, error)) return false;
  proposals.swap(merged);
  return true;
}

bool chooseProposal(RegionProposal &region, int candidate, bool lock) {
  if (region.protectedRegion || candidate < -2 ||
      candidate >= region.candidates.size()) return false;
  region.selected = candidate;
  region.locked = lock && candidate != -1;
  return true;
}

void protectProposal(RegionProposal &region, bool protect) {
  region.protectedRegion = protect;
}

void invalidateIncompleteConsensus(QVector<RegionProposal> &proposals,
                                    int expectedReferenceCount) {
  const bool bounded = expectedReferenceCount > 0 &&
                       expectedReferenceCount <= MaximumProposalReferences;
  auto complete = [expectedReferenceCount, bounded](const QVector<int> &values) {
    if (!bounded || values.size() != expectedReferenceCount) return false;
    QSet<int> seen;
    if (!indices(values, seen)) return false;
    for (int reference = 0; reference < expectedReferenceCount; ++reference)
      if (!seen.contains(reference)) return false;
    return true;
  };
  for (RegionProposal &region : proposals) {
    if (region.locked || region.protectedRegion || region.selected < 0) continue;
    if (region.selected >= region.candidates.size()) {
      region.selected = -1;
      continue;
    }
    const ProposalCandidate &candidate = region.candidates[region.selected];
    if (candidate.referenceIndex == -1 && candidate.referenceRegion == -1) continue;
    if (!complete(region.references) || !complete(candidate.supportingReferences))
      region.selected = -1;
  }
}

qint64 proposalMemoryBytes(const QVector<RegionProposal> &proposals) {
  qint64 bytes = qint64(proposals.capacity()) * qint64(sizeof(RegionProposal));
  for (const RegionProposal &region : proposals) {
    bytes += qint64(region.references.capacity()) * qint64(sizeof(int));
    bytes += qint64(region.candidates.capacity()) * qint64(sizeof(ProposalCandidate));
    for (const ProposalCandidate &candidate : region.candidates)
      bytes += qint64(candidate.supportingReferences.capacity()) * qint64(sizeof(int));
  }
  return bytes;
}

}  // namespace Cadmium
