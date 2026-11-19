#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityIdentity.h"
#include "Data/Enums/FlecsSpatialMetadataDiagnosticCode.h"

namespace FlecsSimulation
{
struct FSpatialMetadataDiagnostic
	{
		FNetworkEntityIdentity Identity;
		ESpatialMetadataDiagnosticCode Code = ESpatialMetadataDiagnosticCode::MissingDefinitionPolicy;
		/** Nonzero only for the final bounded truncation summary entry. */
		uint32 OmittedDiagnosticCount = 0;
	};
}
