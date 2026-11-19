#include "Networking/FlecsSessionManifestEnvelope.h"

#include "ModRegistry/ModSchemaRegistry.h"

bool FFlecsSessionManifestEnvelope::Encode(
	uint32 EntityCapacity, TConstArrayView<uint8> Manifest, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	if (EntityCapacity == 0 || EntityCapacity > MaxNetworkEntities || Manifest.IsEmpty()
		|| Manifest.Num() > static_cast<int32>(FlecsModRegistry::MaxManifestDescriptorBytes) - HeaderBytes)
	{
		return false;
	}
	OutBytes.Reserve(HeaderBytes + Manifest.Num());
	const uint8 Header[] = { 'F', 'L', 'C', 'P', 0, 0, 0, 1 };
	OutBytes.Append(Header, UE_ARRAY_COUNT(Header));
	OutBytes.Add(static_cast<uint8>(EntityCapacity >> 24));
	OutBytes.Add(static_cast<uint8>(EntityCapacity >> 16));
	OutBytes.Add(static_cast<uint8>(EntityCapacity >> 8));
	OutBytes.Add(static_cast<uint8>(EntityCapacity));
	OutBytes.Append(Manifest.GetData(), Manifest.Num());
	return true;
}

bool FFlecsSessionManifestEnvelope::Decode(
	TConstArrayView<uint8> Bytes, uint32& OutEntityCapacity, TArray<uint8>& OutManifest)
{
	OutEntityCapacity = 0;
	OutManifest.Reset();
	if (Bytes.Num() <= HeaderBytes || Bytes.Num() > static_cast<int32>(FlecsModRegistry::MaxManifestDescriptorBytes)
		|| Bytes[0] != 'F' || Bytes[1] != 'L' || Bytes[2] != 'C' || Bytes[3] != 'P'
		|| Bytes[4] != 0 || Bytes[5] != 0 || Bytes[6] != 0 || Bytes[7] != 1)
	{
		return false;
	}
	const uint32 Capacity = (static_cast<uint32>(Bytes[8]) << 24) | (static_cast<uint32>(Bytes[9]) << 16)
		| (static_cast<uint32>(Bytes[10]) << 8) | static_cast<uint32>(Bytes[11]);
	if (Capacity == 0 || Capacity > MaxNetworkEntities)
		return false;
	OutEntityCapacity = Capacity;
	OutManifest.Append(Bytes.GetData() + HeaderBytes, Bytes.Num() - HeaderBytes);
	return true;
}

bool FFlecsSessionManifestEnvelope::DecodeMatching(TConstArrayView<uint8> Bytes, uint32 ExpectedCapacity,
	TArray<uint8>& OutManifest, FString& OutError)
{
	OutError.Reset();
	uint32 RemoteCapacity = 0;
	if (!Decode(Bytes, RemoteCapacity, OutManifest))
	{
		OutError = TEXT("Invalid or unsupported Flecs session capacity envelope.");
		return false;
	}
	if (ExpectedCapacity == 0 || ExpectedCapacity > MaxNetworkEntities || RemoteCapacity != ExpectedCapacity)
	{
		OutManifest.Reset();
		OutError = FString::Printf(TEXT("Flecs network entity capacity mismatch: local %u, remote %u."),
			ExpectedCapacity, RemoteCapacity);
		return false;
	}
	return true;
}
