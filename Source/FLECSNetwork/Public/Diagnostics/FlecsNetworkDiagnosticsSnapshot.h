#pragma once
#include "CoreMinimal.h"

/** Per-world logical payload totals and most recently completed one-second rates. */
struct FLECSNETWORK_API FFlecsNetworkDiagnosticsSnapshot
{
 uint64 Sent[3] = {};
 uint64 Received[3] = {};
 double SentBytesPerSecond[3] = {};
 double ReceivedBytesPerSecond[3] = {};
 uint64 NetworkBufferBytes = 0;
 uint32 Relevant = 0;
 uint32 Selected = 0;
 uint32 Deferred = 0;
};
