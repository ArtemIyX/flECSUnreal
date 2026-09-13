#pragma once

#include "CoreMinimal.h"

#include "FlecsEntityHandle.generated.h"

USTRUCT(BlueprintType)
struct FLECS_API FFlecsEntityHandle
{
	GENERATED_BODY()

public:
	FFlecsEntityHandle() = default;

	bool IsValid() const { return EntityId != 0 && WorldSerial != 0; }
	void Reset()
	{
		EntityId = 0;
		WorldSerial = 0;
	}

	bool operator==(const FFlecsEntityHandle& Other) const
	{
		return EntityId == Other.EntityId && WorldSerial == Other.WorldSerial;
	}

	bool operator!=(const FFlecsEntityHandle& Other) const { return !(*this == Other); }

private:
	friend class UFlecsGameInstanceSubsystem;

	FFlecsEntityHandle(uint64 InEntityId, uint64 InWorldSerial)
		: EntityId(InEntityId)
		, WorldSerial(InWorldSerial)
	{
	}

	UPROPERTY()
	uint64 EntityId = 0;

	UPROPERTY()
	uint64 WorldSerial = 0;
};
