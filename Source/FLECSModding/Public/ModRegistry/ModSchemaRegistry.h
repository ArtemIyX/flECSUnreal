#pragma once

#include "CoreMinimal.h"

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

	enum class EEntryKind : uint8 { Component, EntityDefinition, Command };
	enum class EVisibilityPolicy : uint8 { Relevant, OwnerOnly, Always };

	struct FVersion
	{
		uint16 Major = 1;
		uint16 Minor = 0;
		uint16 Patch = 0;
	};

	struct FComponentSchema
	{
		FString StableId;
		FVersion Version;
		uint32 MaxEncodedBytes = 0;
		TArray<FString> CompatibleWith;
		/** Empty permits any definition that includes this component; otherwise this is an allowlist. */
		TArray<FString> AllowedEntityDefinitions;
		EVisibilityPolicy Visibility = EVisibilityPolicy::Relevant;
		float UpdateRateHz = 15.0f;
		int32 Priority = 0;
		// These callbacks execute on the owning simulation worker. They receive only
		// opaque entity tokens and bounded encoded bytes; they must not access UObjects.
		TFunction<bool(uint64 EntityToken, TArray<uint8>& OutBytes)> Extract;
		TFunction<bool(uint64 EntityToken, TConstArrayView<uint8> Bytes)> Apply;
		TFunction<void(uint64 EntityToken)> Remove;
	};

	struct FEntityDefinition
	{
		FString StableId;
		FVersion Version;
		TArray<FString> ComponentIds;
		uint32 MaxInitialStateBytes = 0;
		TFunction<bool(uint64 EntityToken)> Create;
	};

	struct FCommandSchema
	{
		FString StableId;
		FVersion Version;
		uint32 MaxPayloadBytes = 0;
		FString RequiredPermission;
		// Optional worker-only preflight; must not mutate state. False rejects the command.
		TFunction<bool(uint64 ConnectionToken, uint64 TargetToken, TConstArrayView<uint8> Payload)> Validate;
		// A false result may follow mutation and therefore invalidates the authoritative session.
		TFunction<bool(uint64 ConnectionToken, uint64 TargetToken, TConstArrayView<uint8> Payload)> Execute;
	};

	struct FManifestEntry
	{
		EEntryKind Kind = EEntryKind::Component;
		FString StableId;
		FVersion Version;
		uint32 MaxPayloadBytes = 0;
		FString Metadata;
	};

	/** Plain, bounded manifest value suitable for serializing across a network boundary. */
	struct FLECSMODDING_API FManifestDescriptor
	{
		uint32 ProtocolVersion = 0;
		TArray<FManifestEntry> Entries;

		bool Validate(FString& OutError) const;
		bool Serialize(TArray<uint8>& OutBytes, FString& OutError) const;
		static bool Deserialize(TConstArrayView<uint8> Bytes, FManifestDescriptor& OutDescriptor, FString& OutError);
	};

	struct FSessionWireEntry
	{
		EEntryKind Kind = EEntryKind::Component;
		FString StableId;
	};

	/** Immutable IDs for one peer, issued only by a successful exact comparison. */
	class FLECSMODDING_API FSessionMapping
	{
	public:
		bool IsValid() const { return bValidated; }
		bool FindWireId(EEntryKind Kind, const FString& StableId, uint16& OutId) const;
		bool FindEntryByWireId(uint16 WireId, FSessionWireEntry& OutEntry) const;

	private:
		friend class FRegistry;
		TMap<FString, uint16> WireIds;
		TMap<uint16, FSessionWireEntry> EntriesByWireId;
		bool bValidated = false;
	};

	struct FCompatibilityResult
	{
		bool bCompatible = false;
		TArray<FString> Diagnostics;
		FSessionMapping SessionMapping;
	};

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
