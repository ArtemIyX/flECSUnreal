#pragma once

#include "CoreMinimal.h"

enum class EFlecsDataPublicationAddResult : uint8
{
	Accepted,
	InvalidType,
	InvalidPayload,
	ItemTooLarge,
	ItemLimitReached,
	ByteLimitReached
};
