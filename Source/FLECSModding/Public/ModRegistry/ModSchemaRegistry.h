#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsModVersion.h"
#include "Data/Structs/FlecsComponentSchema.h"
#include "Data/Structs/FlecsEntityDefinition.h"
#include "Data/Structs/FlecsCommandSchema.h"
#include "Data/Structs/FlecsManifestEntry.h"
#include "Data/Structs/FlecsManifestDescriptor.h"
#include "Data/Structs/FlecsSessionWireEntry.h"
#include "Data/Structs/FlecsCompatibilityResult.h"
#include "ModRegistry/FlecsSessionMapping.h"
#include "Data/Enums/FlecsModEntryKind.h"
#include "Data/Enums/FlecsModVisibilityPolicy.h"

/** Thread-safe-to-read immutable metadata and worker-only callbacks registered before a network session. */
namespace FlecsModRegistry
{
constexpr uint32 MaxStableIdLength = 128;
constexpr uint32 MaxManifestEntries = 4096;
constexpr uint32 MaxComponentPayloadBytes = 64 * 1024;
constexpr uint32 MaxCommandPayloadBytes = 16 * 1024;
constexpr uint32 MaxComponentsPerDefinition = 256;
constexpr uint32 MaxSchemaReferences = 256;
constexpr uint32 MaxManifestMetadataBytes = 32768;
constexpr uint32 MaxManifestDescriptorBytes = 1024 * 1024;

/**
	 * Session registry. Registration is mutable only until Freeze(); after that,
	 * canonical manifest and compact IDs remain stable for the session lifetime.
	 * Registering callbacks is a native-code trust boundary: callback code must be
	 * loaded for the session and worker-safe. This type never invokes UObject APIs.
	 */
class FLECSMODDING_API FRegistry
{
public:
	bool RegisterComponent(FComponentSchema Schema, FString& OutError);
	bool RegisterEntityDefinition(FEntityDefinition Definition, FString& OutError);
	bool RegisterCommand(FCommandSchema Command, FString& OutError);
	bool Freeze(uint32 ProtocolVersion, FString& OutError);

	bool IsFrozen() const { return bFrozen; }
	const FString& GetCanonicalManifest() const { return CanonicalManifest; }
	const TArray<FManifestEntry>& GetManifestEntries() const { return ManifestEntries; }
	bool ExportManifestDescriptor(FManifestDescriptor& OutDescriptor, FString& OutError) const;
	/** Compare complete frozen manifests and return a peer-scoped mapping only on exact compatibility. */
	FCompatibilityResult CompareRequired(const FRegistry& Remote) const;
	FCompatibilityResult CompareRequired(const FManifestDescriptor& Remote) const;
	const FComponentSchema* FindComponent(const FString& StableId) const;
	const FEntityDefinition* FindEntityDefinition(const FString& StableId) const;
	const FCommandSchema* FindCommand(const FString& StableId) const;

	static bool IsValidStableId(const FString& StableId);

private:
	bool CanRegister(const FString& StableId, FString& OutError) const;
	static FString KindName(EEntryKind Kind);
	static FString VersionString(const FVersion& Version);
	static FString Escape(const FString& Value);
	static bool SameVersion(const FVersion& A, const FVersion& B);
	void BuildManifest(uint32 ProtocolVersion);

	TMap<FString, FComponentSchema> Components;
	TMap<FString, FEntityDefinition> EntityDefinitions;
	TMap<FString, FCommandSchema> Commands;
	TMap<FString, uint16> SessionWireIds;
	TArray<FManifestEntry> ManifestEntries;
	FString CanonicalManifest;
	uint32 FrozenProtocolVersion = 0;
	bool bFrozen = false;
};
}
