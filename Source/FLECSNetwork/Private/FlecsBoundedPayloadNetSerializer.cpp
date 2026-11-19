#include "FlecsBoundedPayloadNetSerializer.h"

#include "FlecsBoundedPayload.h"
#include "Iris/ReplicationState/PropertyNetSerializerInfoRegistry.h"
#include "Iris/Serialization/NetBitStreamReader.h"
#include "Iris/Serialization/NetBitStreamWriter.h"
#include "Iris/Serialization/NetErrorContext.h"
#include "Iris/Serialization/NetSerializerDelegates.h"

namespace UE::Net
{
	struct FFlecsBoundedPayloadQuantized
	{
		uint16 NumBytes;
		uint8 Bytes[FFlecsBoundedPayload::Capacity];
	};
}

template<>
struct TIsPODType<UE::Net::FFlecsBoundedPayloadQuantized>
{
	enum { Value = true };
};

namespace UE::Net
{
	struct FFlecsBoundedPayloadNetSerializer
	{
		static constexpr uint32 Version = 0;
		static constexpr bool bUseSerializerIsEqual = true;
		typedef ::FFlecsBoundedPayload SourceType;
		typedef FFlecsBoundedPayloadQuantized QuantizedType;
		typedef FNetSerializerConfig ConfigType;
		inline static const ConfigType DefaultConfig;

		static void Serialize(FNetSerializationContext& Context, const FNetSerializeArgs& Args);
		static void Deserialize(FNetSerializationContext& Context, const FNetDeserializeArgs& Args);
		static void Quantize(FNetSerializationContext& Context, const FNetQuantizeArgs& Args);
		static void Dequantize(FNetSerializationContext& Context, const FNetDequantizeArgs& Args);
		static bool IsEqual(FNetSerializationContext& Context, const FNetIsEqualArgs& Args);
		static bool Validate(FNetSerializationContext& Context, const FNetValidateArgs& Args);
	};

	UE_NET_IMPLEMENT_SERIALIZER(FFlecsBoundedPayloadNetSerializer);
	UE_NET_IMPLEMENT_FORWARDING_NETSERIALIZER_AND_REGISTRY_DELEGATES(FlecsBoundedPayload, FFlecsBoundedPayloadNetSerializer);

	void FFlecsBoundedPayloadNetSerializer::Serialize(FNetSerializationContext& Context, const FNetSerializeArgs& Args)
	{
		const QuantizedType& Value = *reinterpret_cast<const QuantizedType*>(Args.Source);
		if (Value.NumBytes > FFlecsBoundedPayload::Capacity)
		{
			Context.SetError(GNetError_InvalidValue);
			return;
		}

		FNetBitStreamWriter* Writer = Context.GetBitStreamWriter();
		Writer->WriteBits(Value.NumBytes, 11U);
		for (uint16 Index = 0; Index < Value.NumBytes; ++Index)
		{
			Writer->WriteBits(Value.Bytes[Index], 8U);
		}
	}

	void FFlecsBoundedPayloadNetSerializer::Deserialize(FNetSerializationContext& Context, const FNetDeserializeArgs& Args)
	{
		QuantizedType& Value = *reinterpret_cast<QuantizedType*>(Args.Target);
		Value = QuantizedType{};

		FNetBitStreamReader* Reader = Context.GetBitStreamReader();
		const uint32 NumBytes = Reader->ReadBits(11U);
		if (Reader->IsOverflown())
		{
			Context.SetError(GNetError_BitStreamOverflow);
			return;
		}
		if (NumBytes > FFlecsBoundedPayload::Capacity)
		{
			Context.SetError(GNetError_InvalidValue);
			return;
		}

		Value.NumBytes = static_cast<uint16>(NumBytes);
		for (uint16 Index = 0; Index < Value.NumBytes; ++Index)
		{
			Value.Bytes[Index] = static_cast<uint8>(Reader->ReadBits(8U));
			if (Reader->IsOverflown())
			{
				Value = QuantizedType{};
				Context.SetError(GNetError_BitStreamOverflow);
				return;
			}
		}
	}

	void FFlecsBoundedPayloadNetSerializer::Quantize(FNetSerializationContext& Context, const FNetQuantizeArgs& Args)
	{
		const SourceType& Source = *reinterpret_cast<const SourceType*>(Args.Source);
		QuantizedType& Target = *reinterpret_cast<QuantizedType*>(Args.Target);
		Target = QuantizedType{};
		if (!Source.IsValid())
		{
			Context.SetError(GNetError_InvalidValue);
			return;
		}
		Target.NumBytes = Source.NumBytes;
		if (Target.NumBytes > 0)
		{
			FMemory::Memcpy(Target.Bytes, Source.Bytes, Target.NumBytes);
		}
	}

	void FFlecsBoundedPayloadNetSerializer::Dequantize(FNetSerializationContext& Context, const FNetDequantizeArgs& Args)
	{
		const QuantizedType& Source = *reinterpret_cast<const QuantizedType*>(Args.Source);
		SourceType& Target = *reinterpret_cast<SourceType*>(Args.Target);
		Target.Reset();
		if (Source.NumBytes > FFlecsBoundedPayload::Capacity)
		{
			Context.SetError(GNetError_InvalidValue);
			return;
		}
		Target.NumBytes = Source.NumBytes;
		if (Source.NumBytes > 0)
		{
			FMemory::Memcpy(Target.Bytes, Source.Bytes, Source.NumBytes);
		}
	}

	bool FFlecsBoundedPayloadNetSerializer::IsEqual(FNetSerializationContext& Context, const FNetIsEqualArgs& Args)
	{
		(void)Context;
		if (Args.bStateIsQuantized)
		{
			const QuantizedType& A = *reinterpret_cast<const QuantizedType*>(Args.Source0);
			const QuantizedType& B = *reinterpret_cast<const QuantizedType*>(Args.Source1);
			return A.NumBytes <= FFlecsBoundedPayload::Capacity && A.NumBytes == B.NumBytes &&
				(A.NumBytes == 0 || FMemory::Memcmp(A.Bytes, B.Bytes, A.NumBytes) == 0);
		}

		const SourceType& A = *reinterpret_cast<const SourceType*>(Args.Source0);
		const SourceType& B = *reinterpret_cast<const SourceType*>(Args.Source1);
		return A == B;
	}

	bool FFlecsBoundedPayloadNetSerializer::Validate(FNetSerializationContext& Context, const FNetValidateArgs& Args)
	{
		(void)Context;
		return reinterpret_cast<const SourceType*>(Args.Source)->IsValid();
	}
}
