#pragma once

#include "CoreMinimal.h"
#include "Data/Enums/FlecsTickPhase.h"
#include "Engine/DeveloperSettings.h"
#include "FlecsDeveloperSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="FLECS"))
class FLECS_API UFlecsDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override
	{
		return FName(TEXT("Plugins"));
	}

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Plugin")
	EFlecsTickPhase TickPhase = EFlecsTickPhase::PostPhysics;
};
