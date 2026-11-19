#pragma once
#include "CoreMinimal.h"

/** Bounded high-watermark for monotonically ordered logical command sequences. */
struct FLECSNETWORK_API FFlecsLogicalSequence
{
 uint64 Epoch = 0;
 uint32 LastSequence = 0;
 bool Accept(uint64 InEpoch, uint32 Sequence)
 {
  if (InEpoch == 0 || Sequence == 0) return false;
  if (Epoch != InEpoch) { Epoch = InEpoch; LastSequence = 0; }
  const uint32 Delta = Sequence - LastSequence;
  if (LastSequence != 0 && (Delta == 0 || Delta >= 0x80000000u)) return false;
  LastSequence = Sequence;
  return true;
 }
};
