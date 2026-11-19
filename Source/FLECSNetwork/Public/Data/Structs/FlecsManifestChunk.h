#pragma once

#include "CoreMinimal.h"
#include "FlecsBoundedPayload.h"
#include "FlecsManifestChunk.generated.h"

/** Bounded chunk used only by the pre-session manifest handshake. */
USTRUCT()
struct FLECSNETWORK_API FFlecsManifestChunk
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 WorldEpoch = 0;
	UPROPERTY()
	uint32 TransferId = 0;
	UPROPERTY()
	uint16 ChunkIndex = 0;
	UPROPERTY()
	uint16 ChunkCount = 0;
	UPROPERTY()
	uint32 TotalBytes = 0;
	UPROPERTY()
	FFlecsBoundedPayload Payload;
};
