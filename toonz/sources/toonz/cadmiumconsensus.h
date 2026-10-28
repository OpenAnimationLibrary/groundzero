#pragma once

#include "cadmiumtypes.h"

#include <QJsonObject>

namespace Cadmium {

constexpr int MaximumProposalRegions = 65537;
constexpr int MaximumProposalReferences = 32;
constexpr int MaximumRegionCandidates = 512;
constexpr int MaximumProposalCandidates = 1048576;

struct ProposalCandidate {
  QColor color;
  int style = -1;
  NativeColor nativeColor;
  // Paired negative origins identify an artist-authored fill.
  int referenceIndex = -1;
  int referenceRegion = -1;
  QVector<int> supportingReferences;
};

struct RegionProposal {
  QVector<ProposalCandidate> candidates;
  // Negative values are unresolved (-1) or explicitly skipped (-2).
  int selected = -1;
  bool locked = false;
  bool protectedRegion = false;
  QVector<int> references;
};

bool sameCandidate(const ProposalCandidate &, const ProposalCandidate &, Kind);
bool validateProposals(const QVector<RegionProposal> &, Kind, QString &error);
bool parseReferenceProposals(const QJsonObject &response,
                             const Frame &reference, const Frame &target,
                             int referenceIndex,
                             QVector<RegionProposal> &proposals,
                             QString &error);
bool mergeReferenceProposals(QVector<RegionProposal> &proposals,
                             const QVector<RegionProposal> &referenceProposals,
                             Kind, QString &error);
bool chooseProposal(RegionProposal &, int candidate, bool lock = true);
void protectProposal(RegionProposal &, bool protect);
qint64 proposalMemoryBytes(const QVector<RegionProposal> &);
void invalidateIncompleteConsensus(QVector<RegionProposal> &,
                                    int expectedReferenceCount);

}  // namespace Cadmium
