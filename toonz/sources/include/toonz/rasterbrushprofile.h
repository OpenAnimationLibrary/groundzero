#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace RasterBrush {
// Versioned classification of existing implementations, never a setting-level
// source admission or reference-verification decision. IDs remain the exact
// serialized profile strings. The descriptor itself is not serialized.
enum class ProfileIdV1 : std::uint8_t {
  Unknown, Native, HardPixel, DefaultCircle, DefaultRectangle, Airbrush,
  GeneratedMask, Scatter, FixedBitmap, BitmapMask, RandomBitmap,
  LegacySmear, LegacyDulling, NewSmear, NewDulling
};
enum class ProfileFamilyV1 { Unknown, Native, SourcePixel, SourceBitmap, SourceWet };
enum class TipFamilyV1 { Unknown, Native, HardPixel, DefaultCircle, DefaultRectangle, GeneratedMask, Bitmap, BitmapMask };
enum class SchedulingFamilyV1 {
  Unknown, NativeDistanceTime, SourceDistanceEndpointDeferred,
  SourceFuturePixel, SourceWetPickupThenFuture, SourceDeliveredTimer
};
enum class TapBehaviorV1 { Unknown, ImmediateDeposit, DeferredDeposit, PickupInitialization };
enum class StrokeSeedPolicyV1 { ResolvedOnly, NativeUserPolicy, OpaqueSourceHostPolicy };
enum class StochasticProcessV1 { NoProcess, Native, SourceScatter, SourceGihProposalCache };
// Vocabulary for subsequent component composition/rejection reports. These
// categories confer no permission: validate()/eligibleFixedSource() retain all
// numeric, resource, active-setting and combination guards.
enum class SourceCapabilityV1 {
  TipResource, Geometry, Sensors, Transfer, Texture, SecondaryMask,
  Foreground, Timing, StochasticProcess, Composition, ReferenceEvidence
};
enum class ProfileRejectionV1 {
  NoRejection, UnknownProfile, UnsupportedSetting, UnimplementedCombination,
  MissingSourceEvidence
};
struct ProfileDescriptorV1 {
  unsigned schemaVersion = 1;
  ProfileIdV1 id = ProfileIdV1::Unknown;
  std::string_view name;
  ProfileFamilyV1 family = ProfileFamilyV1::Unknown;
  TipFamilyV1 tip = TipFamilyV1::Unknown;
  SchedulingFamilyV1 scheduling = SchedulingFamilyV1::Unknown;
  TapBehaviorV1 tap = TapBehaviorV1::Unknown;
  StrokeSeedPolicyV1 strokeSeed = StrokeSeedPolicyV1::ResolvedOnly;
  StochasticProcessV1 stochastic = StochasticProcessV1::NoProcess;

  constexpr bool recognized() const { return id != ProfileIdV1::Unknown; }
  constexpr bool isNative() const { return family == ProfileFamilyV1::Native; }
  constexpr bool isSourceCandidate() const {
    return recognized() && !isNative();
  }
  constexpr ProfileRejectionV1 recognitionRejection() const {
    return recognized() ? ProfileRejectionV1::NoRejection : ProfileRejectionV1::UnknownProfile;
  }
  constexpr bool permitsStrokeSeedVariation() const {
    return strokeSeed != StrokeSeedPolicyV1::ResolvedOnly;
  }
  constexpr bool permitsSeedPolicyEditing() const {
    return strokeSeed == StrokeSeedPolicyV1::NativeUserPolicy;
  }
  constexpr bool usesDeliveredTimer() const {
    return scheduling == SchedulingFamilyV1::SourceDeliveredTimer;
  }
  constexpr bool needsDeferredTapDedup(bool sourceMasked) const {
    return tap == TapBehaviorV1::DeferredDeposit && !sourceMasked;
  }
};

inline constexpr std::array<ProfileDescriptorV1, 14> profileDescriptorsV1{{
  {1, ProfileIdV1::Native, "groundzero-native-1", ProfileFamilyV1::Native,
   TipFamilyV1::Native, SchedulingFamilyV1::NativeDistanceTime,
   TapBehaviorV1::ImmediateDeposit, StrokeSeedPolicyV1::NativeUserPolicy, StochasticProcessV1::Native},
  {1, ProfileIdV1::HardPixel, "krita-5.3.4.1-hard-pixel-candidate-1", ProfileFamilyV1::SourcePixel,
   TipFamilyV1::HardPixel, SchedulingFamilyV1::SourceFuturePixel, TapBehaviorV1::DeferredDeposit},
  {1, ProfileIdV1::DefaultCircle, "krita-5.3.4.1-default-circle-candidate-1", ProfileFamilyV1::SourcePixel,
   TipFamilyV1::DefaultCircle, SchedulingFamilyV1::SourceDistanceEndpointDeferred, TapBehaviorV1::ImmediateDeposit},
  {1, ProfileIdV1::DefaultRectangle, "krita-5.3.4.1-default-rect-candidate-1", ProfileFamilyV1::SourcePixel,
   TipFamilyV1::DefaultRectangle, SchedulingFamilyV1::SourceDistanceEndpointDeferred, TapBehaviorV1::ImmediateDeposit},
  {1, ProfileIdV1::Airbrush, "krita-5.3.4.1-airbrush-candidate-1", ProfileFamilyV1::SourcePixel,
   TipFamilyV1::HardPixel, SchedulingFamilyV1::SourceDeliveredTimer, TapBehaviorV1::ImmediateDeposit},
  {1, ProfileIdV1::GeneratedMask, "krita-5.3.4.1-generated-mask-candidate-1", ProfileFamilyV1::SourcePixel,
   TipFamilyV1::GeneratedMask, SchedulingFamilyV1::SourceDistanceEndpointDeferred, TapBehaviorV1::ImmediateDeposit},
  {1, ProfileIdV1::Scatter, "krita-5.3.4.1-scatter-candidate-1", ProfileFamilyV1::SourcePixel,
   TipFamilyV1::HardPixel, SchedulingFamilyV1::SourceFuturePixel, TapBehaviorV1::DeferredDeposit,
   StrokeSeedPolicyV1::OpaqueSourceHostPolicy, StochasticProcessV1::SourceScatter},
  {1, ProfileIdV1::FixedBitmap, "krita-5.3.4.1-fixed-bitmap-candidate-1", ProfileFamilyV1::SourceBitmap,
   TipFamilyV1::Bitmap, SchedulingFamilyV1::SourceDistanceEndpointDeferred, TapBehaviorV1::ImmediateDeposit},
  {1, ProfileIdV1::BitmapMask, "krita-5.3.4.1-fixed-bitmap-mask-candidate-1", ProfileFamilyV1::SourceBitmap,
   TipFamilyV1::BitmapMask, SchedulingFamilyV1::SourceDistanceEndpointDeferred, TapBehaviorV1::ImmediateDeposit},
  {1, ProfileIdV1::RandomBitmap, "krita-5.3.4.1-random-bitmap-candidate-1", ProfileFamilyV1::SourceBitmap,
   TipFamilyV1::Bitmap, SchedulingFamilyV1::SourceFuturePixel, TapBehaviorV1::DeferredDeposit,
   StrokeSeedPolicyV1::OpaqueSourceHostPolicy, StochasticProcessV1::SourceGihProposalCache},
  {1, ProfileIdV1::LegacySmear, "krita-5.3.4.1-legacy-smear-candidate-1", ProfileFamilyV1::SourceWet,
   TipFamilyV1::HardPixel, SchedulingFamilyV1::SourceWetPickupThenFuture, TapBehaviorV1::PickupInitialization},
  {1, ProfileIdV1::LegacyDulling, "krita-5.3.4.1-legacy-dulling-candidate-1", ProfileFamilyV1::SourceWet,
   TipFamilyV1::HardPixel, SchedulingFamilyV1::SourceWetPickupThenFuture, TapBehaviorV1::PickupInitialization},
  {1, ProfileIdV1::NewSmear, "krita-5.3.4.1-new-smear-candidate-1", ProfileFamilyV1::SourceWet,
   TipFamilyV1::HardPixel, SchedulingFamilyV1::SourceWetPickupThenFuture, TapBehaviorV1::PickupInitialization},
  {1, ProfileIdV1::NewDulling, "krita-5.3.4.1-new-dulling-candidate-1", ProfileFamilyV1::SourceWet,
   TipFamilyV1::HardPixel, SchedulingFamilyV1::SourceWetPickupThenFuture, TapBehaviorV1::PickupInitialization}
}};

constexpr bool profileDescriptorCatalogCompleteV1() {
  for (std::size_t index = 0; index < profileDescriptorsV1.size(); ++index) {
    const auto &profile = profileDescriptorsV1[index];
    if (profile.schemaVersion != 1 || !profile.recognized() || profile.name.empty() ||
        profile.family == ProfileFamilyV1::Unknown || profile.tip == TipFamilyV1::Unknown ||
        profile.scheduling == SchedulingFamilyV1::Unknown || profile.tap == TapBehaviorV1::Unknown ||
        (profile.id == ProfileIdV1::Native) != profile.isNative() ||
        (profile.strokeSeed == StrokeSeedPolicyV1::NativeUserPolicy) != profile.isNative())
      return false;
    const auto expectedTap = profile.scheduling == SchedulingFamilyV1::SourceFuturePixel
        ? TapBehaviorV1::DeferredDeposit
        : profile.scheduling == SchedulingFamilyV1::SourceWetPickupThenFuture
            ? TapBehaviorV1::PickupInitialization : TapBehaviorV1::ImmediateDeposit;
    if (profile.tap != expectedTap) return false;
    const bool opaque = profile.stochastic == StochasticProcessV1::SourceScatter ||
                        profile.stochastic == StochasticProcessV1::SourceGihProposalCache;
    if ((profile.strokeSeed == StrokeSeedPolicyV1::OpaqueSourceHostPolicy) != opaque)
      return false;
    for (std::size_t previous = 0; previous < index; ++previous)
      if (profile.id == profileDescriptorsV1[previous].id ||
          profile.name == profileDescriptorsV1[previous].name)
        return false;
  }
  return true;
}
static_assert(profileDescriptorCatalogCompleteV1(),
              "Profile descriptor requires unique exact IDs and explicit scheduler/tap/seed facets");

constexpr ProfileDescriptorV1 profileDescriptorV1(std::string_view name) {
  for (const auto &profile : profileDescriptorsV1)
    if (profile.name == name) return profile;
  return {};  // Exact allowlist: prefixes, unknown revisions and aliases fail.
}
} // namespace RasterBrush
