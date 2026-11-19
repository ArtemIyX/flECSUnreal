#pragma once

#include "CoreMinimal.h"

struct FFlecsManifestAssembly
{
	uint64 WorldEpoch = 0;
	uint32 TransferId = 0;
	uint32 TotalBytes = 0;
	uint16 ChunkCount = 0;
	uint32 ReceivedBytes = 0;
	TArray<TArray<uint8>> Chunks;
	TBitArray<> ReceivedChunks;
};
