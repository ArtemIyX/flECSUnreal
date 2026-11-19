#pragma once

#include "CoreMinimal.h"

/** Deterministic, worker-safe primitives for application-defined network codecs. */
namespace FlecsQuantization
{
/** Round to nearest integer, with half steps away from zero. Reject non-finite values,
	 * non-positive steps, and results outside int32. On failure, OutValue is unchanged. */
FLECSNETWORK_API bool QuantizeSigned(double Value, double Step, int32& OutValue);
/** Reconstruct a signed value. Reject invalid steps or non-finite results. */
FLECSNETWORK_API bool DequantizeSigned(int32 Value, double Step, double& OutValue);
/** Wrap degrees to [0,360) and round to the nearest of 65536 circular steps. */
FLECSNETWORK_API bool QuantizeAngleDegrees(double Degrees, uint16& OutValue);
/** Reconstruct the canonical angle in [0,360). */
FLECSNETWORK_API double DequantizeAngleDegrees(uint16 Value);

/** Explicit network byte order; readers leave offset and output unchanged on failure. */
FLECSNETWORK_API void WriteInt32(TArray<uint8>& Bytes, int32 Value);
FLECSNETWORK_API void WriteUInt16(TArray<uint8>& Bytes, uint16 Value);
FLECSNETWORK_API bool ReadInt32(TConstArrayView<uint8> Bytes, int32& Offset, int32& OutValue);
FLECSNETWORK_API bool ReadUInt16(TConstArrayView<uint8> Bytes, int32& Offset, uint16& OutValue);
}
