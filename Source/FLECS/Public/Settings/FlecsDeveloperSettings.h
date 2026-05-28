#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FlecsDeveloperSettings.generated.h"

UENUM(BlueprintType)
enum class EFlecsTickPhase : uint8
{
	PrePhysics UMETA(DisplayName = "Pre Physics"),
	DuringPhysics UMETA(DisplayName = "During Physics"),
	PostPhysics UMETA(DisplayName = "Post Physics"),
	PostUpdateWork UMETA(DisplayName = "Post Update Work")
};

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
