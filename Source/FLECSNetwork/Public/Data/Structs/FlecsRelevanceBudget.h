#pragma once

#include "CoreMinimal.h"

struct FLECSNETWORK_API FFlecsRelevanceBudget
{
	uint32 MaxRecords = 128;
	uint32 MaxBytes = 64 * 1024;
};
