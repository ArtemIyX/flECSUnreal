#pragma once
#include "CoreMinimal.h"
#include "Data/Structs/FlecsDistanceUpdateRate.h"

namespace FlecsDiagnostics
{
 /** Return INDEX_NONE for invalid tiers or a distance beyond the last tier. */
 inline int32 FindEstimatedTier(TConstArrayView<FFlecsDistanceUpdateRate> Tiers, double DistanceCm)
 {
  if (!FMath::IsFinite(DistanceCm) || DistanceCm < 0.0 || Tiers.IsEmpty() || Tiers.Num() > 4) return INDEX_NONE;
  float Previous = 0.0f;
  int32 Result = INDEX_NONE;
  for (int32 Index = 0; Index < Tiers.Num(); ++Index)
  {
   const auto& Tier = Tiers[Index];
   if (!FMath::IsFinite(Tier.MaxDistance) || Tier.MaxDistance <= Previous || Tier.MaxDistance > 1000000.0f ||
       !FMath::IsFinite(Tier.UpdateRateHz) || Tier.UpdateRateHz <= 0.0f || Tier.UpdateRateHz > 60.0f) return INDEX_NONE;
   if (Result == INDEX_NONE && DistanceCm <= Tier.MaxDistance) Result = Index;
   Previous = Tier.MaxDistance;
  }
  return Result;
 }
}
