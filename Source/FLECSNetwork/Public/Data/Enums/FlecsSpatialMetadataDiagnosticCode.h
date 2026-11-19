#pragma once

#include "CoreMinimal.h"

namespace FlecsSimulation
{
enum class ESpatialMetadataDiagnosticCode : uint8
{
	MissingDefinitionPolicy,
	NonSpatialSkipped,
	PositionAccessorFailed,
	InvalidPosition,
	UnknownComponentSchema,
	EstimatedUpdateExceedsTransferCap,
	DiagnosticsTruncated
};
}
