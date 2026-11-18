// Input modifiers copy each new point of a stroke into their sub-track. A
// Windows build read the copied point at a wrong index, so the first point of
// every vector Brush stroke crashed. The copy must keep every point as drawn.

#include <tools/inputmanager.h>
#include <tools/modifiers/modifiertangents.h>
#include <tools/track.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

void addPoint(TTrack &track, bool final) {
  const double x = 3.0 * track.size();
  track.push_back(TTrackPoint(TPointD(x, 0.5 * x), 0.2 + 0.01 * track.size(), TPointD(),
                              track.size(), track.size(), 0.0, final),
                  true);
}

void requireCopy(const TTrack &original, const TTrackList &outTracks,
                 const std::string &name) {
  require(outTracks.size() == 1 && outTracks.front(),
          name + ": expected one sub-track");
  const TTrack &copy = *outTracks.front();
  require(copy.original == &original, name + ": sub-track lost its original");
  require(copy.size() == original.size(),
          name + ": sub-track has " + std::to_string(copy.size()) + " points, original has " +
              std::to_string(original.size()));
  for (int i = 0; i < original.size(); ++i) {
    const TTrackPoint &p = copy[i];
    const TTrackPoint &q = original[i];
    require(p.position == q.position && p.pressure == q.pressure &&
                std::abs(p.originalIndex - i) < 1e-9,
            name + ": sub-track point " + std::to_string(i) + " is not original point " +
                std::to_string(i));
  }
  require(copy.finished() == original.finished(),
          name + ": sub-track final state differs from its original");
}

// Grows one stroke in batches, as mouse events do, and checks the copy after
// each batch: the first batch creates the sub-track, later ones extend it.
void checkModifier(TInputModifier &modifier, const std::string &name) {
  TTrackP track = new TTrack();
  const int batches[] = {1, 3, 5};
  for (int batch = 0; batch < 3; ++batch) {
    for (int k = 0; k < batches[batch]; ++k)
      addPoint(*track, batch == 2 && k == batches[batch] - 1);
    TTrackList outTracks;
    modifier.modifyTrack(*track, outTracks);
    requireCopy(*track, outTracks, name + " batch " + std::to_string(batch + 1));
  }
}
}  // namespace

int main() {
  try {
    TInputModifierP base = new TInputModifier();
    checkModifier(*base, "TInputModifier");
    TInputModifierP tangents = new TModifierTangents();
    checkModifier(*tangents, "TModifierTangents");
  } catch (const std::exception &error) {
    std::cerr << "inputmodifier: FAIL: " << error.what() << std::endl;
    return 1;
  }
  std::cout << "inputmodifier: PASS" << std::endl;
  return 0;
}
