#include "ModRegistry/ModSchemaRegistry.h"

namespace FlecsModRegistry
{
	namespace
	{
		bool IsIdCharacter(TCHAR Character)
		{
			return (Character >= TEXT('a') && Character <= TEXT('z')) ||
				(Character >= TEXT('0') && Character <= TEXT('9')) ||
				Character == TEXT('_') || Character == TEXT('-') || Character == TEXT('.');
		}

		FString JoinSorted(const TArray<FString>& Values)
		{
			TArray<FString> Sorted = Values;
			Sorted.Sort([](const FString& A, const FString& B) { return A < B; });
			FString Result;
			for (int32 Index = 0; Index < Sorted.Num(); ++Index)
			{
				if (Index != 0) Result += TEXT(",");
				Result += Sorted[Index];
			}
			return Result;
		}

		FString MakeWireKey(EEntryKind Kind, const FString& Id)
		{
			return FString::Printf(TEXT("%u:%s"), static_cast<uint32>(Kind), *Id);
		}

		bool IsValidKindAndPayload(EEntryKind Kind, uint32 MaxPayloadBytes)
		{
			switch (Kind)
			{
			case EEntryKind::Component:
			case EEntryKind::EntityDefinition:
				return MaxPayloadBytes > 0 && MaxPayloadBytes <= MaxComponentPayloadBytes;
			case EEntryKind::Command:
				return MaxPayloadBytes > 0 && MaxPayloadBytes <= MaxCommandPayloadBytes;
			default:
				return false;
			}
		}

		bool IsValidMetadata(const FString& Metadata)
		{
			if (Metadata.IsEmpty() || Metadata.Len() > static_cast<int32>(MaxManifestMetadataBytes)) return false;
			for (TCHAR Character : Metadata)
			{
				if (Character < 0x20 || Character > 0x7e || Character == TEXT('|')) return false;
			}
			return true;
		}

		bool IsBefore(const FManifestEntry& A, const FManifestEntry& B)
		{
			return A.Kind == B.Kind ? A.StableId < B.StableId : static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind);
		}

		void WriteU16(TArray<uint8>& Bytes, uint16 Value)
		{
			Bytes.Add(static_cast<uint8>(Value & 0xff));
			Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		}

		void WriteU32(TArray<uint8>& Bytes, uint32 Value)
		{
			WriteU16(Bytes, static_cast<uint16>(Value & 0xffff));
			WriteU16(Bytes, static_cast<uint16>((Value >> 16) & 0xffff));
		}

		bool ReadU16(TConstArrayView<uint8> Bytes, int32& Offset, uint16& OutValue)
		{
			if (Offset < 0 || Bytes.Num() - Offset < 2) return false;
			OutValue = static_cast<uint16>(Bytes[Offset]) | (static_cast<uint16>(Bytes[Offset + 1]) << 8);
			Offset += 2;
			return true;
		}

		bool ReadU32(TConstArrayView<uint8> Bytes, int32& Offset, uint32& OutValue)
		{
			uint16 Low = 0;
			uint16 High = 0;
			if (!ReadU16(Bytes, Offset, Low) || !ReadU16(Bytes, Offset, High)) return false;
			OutValue = static_cast<uint32>(Low) | (static_cast<uint32>(High) << 16);
			return true;
		}

		bool ReadAsciiString(TConstArrayView<uint8> Bytes, int32& Offset, uint16 Length, uint32 MaximumLength, FString& OutValue)
		{
			if (Length > MaximumLength || Offset < 0 || Bytes.Num() - Offset < Length) return false;
			OutValue.Reset(Length);
			for (uint16 Index = 0; Index < Length; ++Index)
			{
				const uint8 Character = Bytes[Offset++];
				if (Character < 0x20 || Character > 0x7e) return false;
				OutValue.AppendChar(static_cast<TCHAR>(Character));
			}
			return true;
		}
	}

	bool FManifestDescriptor::Validate(FString& OutError) const
	{
		if (ProtocolVersion == 0) { OutError = TEXT("Manifest protocol version must be nonzero."); return false; }
		if (Entries.Num() > static_cast<int32>(MaxManifestEntries))
		{
			OutError = TEXT("Manifest entry count exceeds the protocol limit.");
			return false;
		}
		const FManifestEntry* Previous = nullptr;
		TSet<FString> UniqueStableIds;
		uint64 EncodedSize = 10;
		for (const FManifestEntry& Entry : Entries)
		{
			if (!FRegistry::IsValidStableId(Entry.StableId) || Entry.Version.Major == 0 ||
				!IsValidKindAndPayload(Entry.Kind, Entry.MaxPayloadBytes) || !IsValidMetadata(Entry.Metadata) ||
				UniqueStableIds.Contains(Entry.StableId))
			{
				OutError = FString::Printf(TEXT("Manifest entry '%s' has a duplicate ID or invalid identity, version, payload bound, kind, or metadata."), *Entry.StableId);
				return false;
			}
			UniqueStableIds.Add(Entry.StableId);
			if (Previous && !IsBefore(*Previous, Entry))
			{
				OutError = TEXT("Manifest entries must be strictly ordered by kind and stable ID without duplicates.");
				return false;
			}
			Previous = &Entry;
			EncodedSize += 15ull + static_cast<uint64>(Entry.StableId.Len()) + static_cast<uint64>(Entry.Metadata.Len());
			if (EncodedSize > MaxManifestDescriptorBytes)
			{
				OutError = TEXT("Manifest descriptor exceeds the encoded byte limit.");
				return false;
			}
		}
		OutError.Reset();
		return true;
	}

	bool FManifestDescriptor::Serialize(TArray<uint8>& OutBytes, FString& OutError) const
	{
		if (!Validate(OutError)) return false;
		OutBytes.Reset();
		uint32 EncodedSize = 10;
		for (const FManifestEntry& Entry : Entries)
		{
			EncodedSize += 15 + static_cast<uint32>(Entry.StableId.Len()) + static_cast<uint32>(Entry.Metadata.Len());
		}
		OutBytes.Reserve(static_cast<int32>(EncodedSize));
		OutBytes.Add(static_cast<uint8>('F'));
		OutBytes.Add(static_cast<uint8>('M'));
		OutBytes.Add(static_cast<uint8>('D'));
		OutBytes.Add(1);
		WriteU32(OutBytes, ProtocolVersion);
		WriteU16(OutBytes, static_cast<uint16>(Entries.Num()));
		for (const FManifestEntry& Entry : Entries)
		{
			OutBytes.Add(static_cast<uint8>(Entry.Kind));
			WriteU16(OutBytes, Entry.Version.Major);
			WriteU16(OutBytes, Entry.Version.Minor);
			WriteU16(OutBytes, Entry.Version.Patch);
			WriteU32(OutBytes, Entry.MaxPayloadBytes);
			WriteU16(OutBytes, static_cast<uint16>(Entry.StableId.Len()));
			WriteU16(OutBytes, static_cast<uint16>(Entry.Metadata.Len()));
			for (TCHAR Character : Entry.StableId) OutBytes.Add(static_cast<uint8>(Character));
			for (TCHAR Character : Entry.Metadata) OutBytes.Add(static_cast<uint8>(Character));
		}
		return true;
	}

	bool FManifestDescriptor::Deserialize(TConstArrayView<uint8> Bytes, FManifestDescriptor& OutDescriptor, FString& OutError)
	{
		if (Bytes.Num() < 10 || Bytes.Num() > static_cast<int32>(MaxManifestDescriptorBytes))
		{
			OutError = TEXT("Manifest descriptor length is outside the allowed bounds.");
			return false;
		}
		if (Bytes[0] != 'F' || Bytes[1] != 'M' || Bytes[2] != 'D' || Bytes[3] != 1)
		{
			OutError = TEXT("Manifest descriptor magic or format version is invalid.");
			return false;
		}
		int32 Offset = 4;
		FManifestDescriptor Candidate;
		uint16 EntryCount = 0;
		if (!ReadU32(Bytes, Offset, Candidate.ProtocolVersion) || !ReadU16(Bytes, Offset, EntryCount) ||
			Candidate.ProtocolVersion == 0 || EntryCount > MaxManifestEntries)
		{
			OutError = TEXT("Manifest descriptor protocol version or entry count is invalid.");
			return false;
		}
		if (static_cast<uint64>(EntryCount) * 15ull > static_cast<uint64>(Bytes.Num() - Offset))
		{
			OutError = TEXT("Manifest descriptor is too short for the declared entry count.");
			return false;
		}
		Candidate.Entries.Reserve(EntryCount);
		for (uint16 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
		{
			if (Bytes.Num() - Offset < 15)
			{
				OutError = TEXT("Manifest descriptor ended inside an entry header.");
				return false;
			}
			FManifestEntry Entry;
			Entry.Kind = static_cast<EEntryKind>(Bytes[Offset++]);
			uint16 IdLength = 0;
			uint16 MetadataLength = 0;
			if (!ReadU16(Bytes, Offset, Entry.Version.Major) || !ReadU16(Bytes, Offset, Entry.Version.Minor) ||
				!ReadU16(Bytes, Offset, Entry.Version.Patch) || !ReadU32(Bytes, Offset, Entry.MaxPayloadBytes) ||
				!ReadU16(Bytes, Offset, IdLength) || !ReadU16(Bytes, Offset, MetadataLength) ||
				IdLength == 0 || IdLength > MaxStableIdLength || MetadataLength == 0 || MetadataLength > MaxManifestMetadataBytes ||
				static_cast<uint32>(IdLength) + static_cast<uint32>(MetadataLength) > static_cast<uint32>(Bytes.Num() - Offset) ||
				!ReadAsciiString(Bytes, Offset, IdLength, MaxStableIdLength, Entry.StableId) ||
				!ReadAsciiString(Bytes, Offset, MetadataLength, MaxManifestMetadataBytes, Entry.Metadata))
			{
				OutError = TEXT("Manifest descriptor entry contains an invalid length or non-ASCII text.");
				return false;
			}
			Candidate.Entries.Add(MoveTemp(Entry));
		}
		if (Offset != Bytes.Num() || !Candidate.Validate(OutError))
		{
			if (Offset != Bytes.Num()) OutError = TEXT("Manifest descriptor contains trailing bytes.");
			return false;
		}
		OutDescriptor = MoveTemp(Candidate);
		return true;
	}

	bool FRegistry::IsValidStableId(const FString& StableId)
	{
		if (StableId.IsEmpty() || StableId.Len() > static_cast<int32>(MaxStableIdLength)) return false;
		int32 ColonIndex = INDEX_NONE;
		for (int32 Index = 0; Index < StableId.Len(); ++Index)
		{
			const TCHAR Character = StableId[Index];
			if (Character == TEXT(':'))
			{
				if (ColonIndex != INDEX_NONE || Index == 0 || Index == StableId.Len() - 1) return false;
				ColonIndex = Index;
			}
			else if (!IsIdCharacter(Character)) return false;
		}
		return ColonIndex > 0 && IsIdCharacter(StableId[0]) && IsIdCharacter(StableId[ColonIndex + 1]);
	}

	bool FRegistry::CanRegister(const FString& StableId, FString& OutError) const
	{
		if (bFrozen) { OutError = TEXT("Registry is frozen for this session."); return false; }
		if (!IsValidStableId(StableId))
		{
			OutError = FString::Printf(TEXT("Stable ID '%s' must be a namespaced lowercase ID such as 'mod:component'."), *StableId);
			return false;
		}
		if (Components.Contains(StableId) || EntityDefinitions.Contains(StableId) || Commands.Contains(StableId))
		{
			OutError = FString::Printf(TEXT("Stable ID collision: '%s' is already registered."), *StableId);
			return false;
		}
		const int32 Count = Components.Num() + EntityDefinitions.Num() + Commands.Num();
		if (Count >= static_cast<int32>(MaxManifestEntries))
		{
			OutError = TEXT("Registry entry limit exceeded.");
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool FRegistry::RegisterComponent(FComponentSchema Schema, FString& OutError)
	{
		if (!CanRegister(Schema.StableId, OutError)) return false;
		if (Schema.Version.Major == 0 || Schema.MaxEncodedBytes == 0 || Schema.MaxEncodedBytes > MaxComponentPayloadBytes ||
			!Schema.Extract || !Schema.Apply || !Schema.Remove || !FMath::IsFinite(Schema.UpdateRateHz) ||
			Schema.UpdateRateHz <= 0.0f || Schema.UpdateRateHz > 60.0f ||
			FMath::RoundToInt(Schema.UpdateRateHz * 1000000.0f) <= 0 ||
			Schema.CompatibleWith.Num() > static_cast<int32>(MaxSchemaReferences) ||
			Schema.AllowedEntityDefinitions.Num() > static_cast<int32>(MaxSchemaReferences))
		{
			OutError = FString::Printf(TEXT("Component '%s' has an invalid version, payload limit, callback, or update rate."), *Schema.StableId);
			return false;
		}
		TSet<FString> UniqueReferences;
		for (const FString& CompatibleId : Schema.CompatibleWith)
		{
			if (!IsValidStableId(CompatibleId) || UniqueReferences.Contains(CompatibleId))
			{
				OutError = TEXT("Component compatibility metadata contains an invalid or duplicate stable ID.");
				return false;
			}
			UniqueReferences.Add(CompatibleId);
		}
		UniqueReferences.Reset();
		for (const FString& DefinitionId : Schema.AllowedEntityDefinitions)
		{
			if (!IsValidStableId(DefinitionId) || UniqueReferences.Contains(DefinitionId))
			{
				OutError = TEXT("Component allowed-definition metadata contains an invalid or duplicate stable ID.");
				return false;
			}
			UniqueReferences.Add(DefinitionId);
		}
		const FString StableId = Schema.StableId;
		Components.Add(StableId, MoveTemp(Schema));
		return true;
	}

	bool FRegistry::RegisterEntityDefinition(FEntityDefinition Definition, FString& OutError)
	{
		if (!CanRegister(Definition.StableId, OutError)) return false;
		if (Definition.Version.Major == 0 || Definition.MaxInitialStateBytes == 0 ||
			Definition.MaxInitialStateBytes > MaxComponentPayloadBytes || !Definition.Create ||
			Definition.ComponentIds.Num() > static_cast<int32>(MaxComponentsPerDefinition))
		{
			OutError = FString::Printf(TEXT("Entity definition '%s' has an invalid version, payload limit, callback, or component count."), *Definition.StableId);
			return false;
		}
		TSet<FString> UniqueIds;
		for (const FString& ComponentId : Definition.ComponentIds)
		{
			if (!IsValidStableId(ComponentId) || UniqueIds.Contains(ComponentId))
			{
				OutError = FString::Printf(TEXT("Entity definition '%s' contains an invalid or duplicate component ID."), *Definition.StableId);
				return false;
			}
			UniqueIds.Add(ComponentId);
		}
		const FString StableId = Definition.StableId;
		EntityDefinitions.Add(StableId, MoveTemp(Definition));
		return true;
	}

	bool FRegistry::RegisterCommand(FCommandSchema Command, FString& OutError)
	{
		if (!CanRegister(Command.StableId, OutError)) return false;
		if (Command.Version.Major == 0 || Command.MaxPayloadBytes == 0 || Command.MaxPayloadBytes > MaxCommandPayloadBytes || !Command.Execute)
		{
			OutError = FString::Printf(TEXT("Command '%s' has an invalid version, payload limit, or worker handler."), *Command.StableId);
			return false;
		}
		if (!IsValidStableId(Command.RequiredPermission))
		{
			OutError = FString::Printf(TEXT("Command '%s' must declare a bounded namespaced permission ID."), *Command.StableId);
			return false;
		}
		const FString StableId = Command.StableId;
		Commands.Add(StableId, MoveTemp(Command));
		return true;
	}

	bool FRegistry::Freeze(uint32 ProtocolVersion, FString& OutError)
	{
		if (bFrozen) { OutError = TEXT("Registry is already frozen."); return false; }
		if (ProtocolVersion == 0) { OutError = TEXT("Protocol version must be nonzero."); return false; }
		for (const TPair<FString, FEntityDefinition>& Pair : EntityDefinitions)
		{
			for (const FString& ComponentId : Pair.Value.ComponentIds)
			{
				const FComponentSchema* Component = Components.Find(ComponentId);
				if (!Component)
				{
					OutError = FString::Printf(TEXT("Entity definition '%s' references unregistered component '%s'."), *Pair.Key, *ComponentId);
					return false;
				}
			}
		}
		for (const TPair<FString, FComponentSchema>& Pair : Components)
		{
			for (const FString& DefinitionId : Pair.Value.AllowedEntityDefinitions)
			{
				if (!EntityDefinitions.Contains(DefinitionId))
				{
					OutError = FString::Printf(TEXT("Component '%s' allows unregistered entity definition '%s'."), *Pair.Key, *DefinitionId);
					return false;
				}
			}
		}
		BuildManifest(ProtocolVersion);
		FrozenProtocolVersion = ProtocolVersion;
		bFrozen = true;
		OutError.Reset();
		return true;
	}

	void FRegistry::BuildManifest(uint32 ProtocolVersion)
	{
		ManifestEntries.Reset();
		for (const TPair<FString, FComponentSchema>& Pair : Components)
		{
			const FComponentSchema& Item = Pair.Value;
			const uint32 UpdateRateMicroHz = static_cast<uint32>(FMath::RoundToInt(Item.UpdateRateHz * 1000000.0f));
			const FString Metadata = FString::Printf(TEXT("max=%u;visibility=%u;rate_microhz=%u;priority=%d;compatible=%s;definitions=%s"),
				Item.MaxEncodedBytes, static_cast<uint32>(Item.Visibility), UpdateRateMicroHz, Item.Priority,
				*JoinSorted(Item.CompatibleWith), *JoinSorted(Item.AllowedEntityDefinitions));
			ManifestEntries.Add({ EEntryKind::Component, Pair.Key, Item.Version, Item.MaxEncodedBytes, Metadata });
		}
		for (const TPair<FString, FEntityDefinition>& Pair : EntityDefinitions)
		{
			ManifestEntries.Add({ EEntryKind::EntityDefinition, Pair.Key, Pair.Value.Version,
				Pair.Value.MaxInitialStateBytes, FString::Printf(TEXT("components=%s"), *JoinSorted(Pair.Value.ComponentIds)) });
		}
		for (const TPair<FString, FCommandSchema>& Pair : Commands)
		{
			ManifestEntries.Add({ EEntryKind::Command, Pair.Key, Pair.Value.Version,
				Pair.Value.MaxPayloadBytes, FString::Printf(TEXT("permission=%s"), *Pair.Value.RequiredPermission) });
		}
		ManifestEntries.StableSort([](const FManifestEntry& A, const FManifestEntry& B)
		{
			if (A.Kind != B.Kind) return static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind);
			return A.StableId < B.StableId;
		});

		CanonicalManifest = FString::Printf(TEXT("protocol=%u\n"), ProtocolVersion);
		SessionWireIds.Reset();
		uint32 NextWireId = 1;
		for (const FManifestEntry& Entry : ManifestEntries)
		{
			CanonicalManifest += FString::Printf(TEXT("%u|%s|%s|%u|%s\n"),
				static_cast<uint32>(Entry.Kind), *Escape(Entry.StableId), *VersionString(Entry.Version),
				Entry.MaxPayloadBytes, *Escape(Entry.Metadata));
			SessionWireIds.Add(MakeWireKey(Entry.Kind, Entry.StableId), static_cast<uint16>(NextWireId++));
		}
	}

	FCompatibilityResult FRegistry::CompareRequired(const FRegistry& Remote) const
	{
		FManifestDescriptor RemoteDescriptor;
		FString Error;
		if (!Remote.ExportManifestDescriptor(RemoteDescriptor, Error))
		{
			FCompatibilityResult Result;
			Result.Diagnostics.Add(Error);
			return Result;
		}
		return CompareRequired(RemoteDescriptor);
	}

	bool FRegistry::ExportManifestDescriptor(FManifestDescriptor& OutDescriptor, FString& OutError) const
	{
		if (!bFrozen)
		{
			OutError = TEXT("Cannot export a manifest before the registry is frozen.");
			return false;
		}
		FManifestDescriptor Candidate;
		Candidate.ProtocolVersion = FrozenProtocolVersion;
		Candidate.Entries = ManifestEntries;
		if (!Candidate.Validate(OutError)) return false;
		OutDescriptor = MoveTemp(Candidate);
		return true;
	}

	FCompatibilityResult FRegistry::CompareRequired(const FManifestDescriptor& Remote) const
	{
		FCompatibilityResult Result;
		if (!bFrozen)
		{
			Result.Diagnostics.Add(TEXT("Local registry must be frozen before compatibility checks."));
			return Result;
		}
		FString ValidationError;
		if (!Remote.Validate(ValidationError))
		{
			Result.Diagnostics.Add(FString::Printf(TEXT("Remote manifest is invalid: %s"), *ValidationError));
			return Result;
		}
		if (FrozenProtocolVersion != Remote.ProtocolVersion)
		{
			Result.Diagnostics.Add(FString::Printf(TEXT("Protocol version mismatch: local %u, remote %u."), FrozenProtocolVersion, Remote.ProtocolVersion));
		}
		int32 LocalIndex = 0;
		int32 RemoteIndex = 0;
		while (LocalIndex < ManifestEntries.Num() && RemoteIndex < Remote.Entries.Num())
		{
			const FManifestEntry& LocalEntry = ManifestEntries[LocalIndex];
			const FManifestEntry& RemoteEntry = Remote.Entries[RemoteIndex];
			if (LocalEntry.Kind == RemoteEntry.Kind && LocalEntry.StableId == RemoteEntry.StableId)
			{
				if (!SameVersion(LocalEntry.Version, RemoteEntry.Version))
				{
					Result.Diagnostics.Add(FString::Printf(TEXT("%s '%s' version mismatch: local %s, remote %s."), *KindName(LocalEntry.Kind), *LocalEntry.StableId,
						*VersionString(LocalEntry.Version), *VersionString(RemoteEntry.Version)));
				}
				if (LocalEntry.MaxPayloadBytes != RemoteEntry.MaxPayloadBytes || LocalEntry.Metadata != RemoteEntry.Metadata)
				{
					Result.Diagnostics.Add(FString::Printf(TEXT("%s '%s' has incompatible payload or policy metadata."), *KindName(LocalEntry.Kind), *LocalEntry.StableId));
				}
				++LocalIndex;
				++RemoteIndex;
			}
			else if (IsBefore(LocalEntry, RemoteEntry))
			{
				Result.Diagnostics.Add(FString::Printf(TEXT("Remote is missing required %s '%s'."), *KindName(LocalEntry.Kind), *LocalEntry.StableId));
				++LocalIndex;
			}
			else
			{
				Result.Diagnostics.Add(FString::Printf(TEXT("Local registry is missing remote %s '%s'."), *KindName(RemoteEntry.Kind), *RemoteEntry.StableId));
				++RemoteIndex;
			}
		}
		for (; LocalIndex < ManifestEntries.Num(); ++LocalIndex)
		{
			const FManifestEntry& Entry = ManifestEntries[LocalIndex];
			Result.Diagnostics.Add(FString::Printf(TEXT("Remote is missing required %s '%s'."), *KindName(Entry.Kind), *Entry.StableId));
		}
		for (; RemoteIndex < Remote.Entries.Num(); ++RemoteIndex)
		{
			const FManifestEntry& Entry = Remote.Entries[RemoteIndex];
			Result.Diagnostics.Add(FString::Printf(TEXT("Local registry is missing remote %s '%s'."), *KindName(Entry.Kind), *Entry.StableId));
		}
		Result.bCompatible = Result.Diagnostics.IsEmpty();
		if (Result.bCompatible)
		{
			Result.SessionMapping.WireIds = SessionWireIds;
			Result.SessionMapping.bValidated = true;
			for (int32 Index = 0; Index < ManifestEntries.Num(); ++Index)
			{
				FSessionWireEntry& WireEntry = Result.SessionMapping.EntriesByWireId.Add(static_cast<uint16>(Index + 1));
				WireEntry.Kind = ManifestEntries[Index].Kind;
				WireEntry.StableId = ManifestEntries[Index].StableId;
			}
		}
		return Result;
	}

	bool FSessionMapping::FindWireId(EEntryKind Kind, const FString& StableId, uint16& OutId) const
	{
		if (!bValidated) return false;
		const uint16* Found = WireIds.Find(MakeWireKey(Kind, StableId));
		if (!Found) return false;
		OutId = *Found;
		return true;
	}

	bool FSessionMapping::FindEntryByWireId(uint16 WireId, FSessionWireEntry& OutEntry) const
	{
		if (!bValidated) return false;
		const FSessionWireEntry* Found = EntriesByWireId.Find(WireId);
		if (!Found) return false;
		OutEntry = *Found;
		return true;
	}

	const FComponentSchema* FRegistry::FindComponent(const FString& StableId) const { return Components.Find(StableId); }
	const FEntityDefinition* FRegistry::FindEntityDefinition(const FString& StableId) const { return EntityDefinitions.Find(StableId); }
	const FCommandSchema* FRegistry::FindCommand(const FString& StableId) const { return Commands.Find(StableId); }

	FString FRegistry::KindName(EEntryKind Kind)
	{
		switch (Kind)
		{
		case EEntryKind::Component: return TEXT("component schema");
		case EEntryKind::EntityDefinition: return TEXT("entity definition");
		case EEntryKind::Command: return TEXT("command schema");
		default: return TEXT("registry entry");
		}
	}

	FString FRegistry::VersionString(const FVersion& Version)
	{
		return FString::Printf(TEXT("%u.%u.%u"), Version.Major, Version.Minor, Version.Patch);
	}

	FString FRegistry::Escape(const FString& Value)
	{
		FString Result = Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("|"), TEXT("\\|"));
		Result.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Result.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		return Result;
	}

	bool FRegistry::SameVersion(const FVersion& A, const FVersion& B)
	{
		return A.Major == B.Major && A.Minor == B.Minor && A.Patch == B.Patch;
	}
}
