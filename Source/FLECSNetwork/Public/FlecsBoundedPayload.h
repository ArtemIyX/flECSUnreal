#pragma once

#include "CoreMinimal.h"
#include "FlecsBoundedPayload.generated.h"

/** Fixed-capacity bytes for Iris RPC and FastArray payloads. Wire decoding never allocates. */
USTRUCT()
struct FLECSNETWORK_API FFlecsBoundedPayload
{
	GENERATED_BODY()

	static constexpr uint16 Capacity = 1024;

	UPROPERTY()
	uint16 NumBytes = 0;

	UPROPERTY()
	uint8 Bytes[1024] = {};

	bool IsValid() const { return NumBytes <= Capacity; }
	int32 Num() const { return IsValid() ? static_cast<int32>(NumBytes) : static_cast<int32>(Capacity) + 1; }
	bool IsEmpty() const { return NumBytes == 0; }
	const uint8* GetData() const { return Bytes; }
	uint8* GetData() { return Bytes; }

	void Reset()
	{
		NumBytes = 0;
		FMemory::Memzero(Bytes, sizeof(Bytes));
	}

	bool SetBytes(const uint8* Source, int32 SourceBytes)
	{
		Reset();
		if (SourceBytes < 0 || SourceBytes > Capacity || (SourceBytes > 0 && Source == nullptr))
		{
			NumBytes = static_cast<uint16>(Capacity + 1);
			return false;
		}
		NumBytes = static_cast<uint16>(SourceBytes);
		if (SourceBytes > 0)
		{
			FMemory::Memcpy(Bytes, Source, static_cast<SIZE_T>(SourceBytes));
		}
		return true;
	}

	bool Append(const uint8* Source, int32 SourceBytes)
	{
		if (!IsValid() || SourceBytes < 0 || SourceBytes > Capacity - NumBytes
			|| (SourceBytes > 0 && Source == nullptr))
		{
			NumBytes = static_cast<uint16>(Capacity + 1);
			return false;
		}
		if (SourceBytes > 0)
		{
			FMemory::Memcpy(Bytes + NumBytes, Source, static_cast<SIZE_T>(SourceBytes));
			NumBytes = static_cast<uint16>(NumBytes + SourceBytes);
		}
		return true;
	}

	FFlecsBoundedPayload& operator=(const TArray<uint8>& Source)
	{
		SetBytes(Source.GetData(), Source.Num());
		return *this;
	}

	operator TArray<uint8>() const
	{
		TArray<uint8> Result;
		if (IsValid() && NumBytes > 0)
		{
			Result.Append(Bytes, NumBytes);
		}
		return Result;
	}

	friend bool operator==(const FFlecsBoundedPayload& A, const FFlecsBoundedPayload& B)
	{
		return A.IsValid() && B.IsValid() && A.NumBytes == B.NumBytes
			&& (A.NumBytes == 0 || FMemory::Memcmp(A.Bytes, B.Bytes, A.NumBytes) == 0);
	}
};
