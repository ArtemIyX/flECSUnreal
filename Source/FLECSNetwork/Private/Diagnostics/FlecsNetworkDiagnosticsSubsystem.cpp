#include "Diagnostics/FlecsNetworkDiagnosticsSubsystem.h"
#include "Diagnostics/FlecsNetworkStats.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FlecsReplicationChannel.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Subsystems/FlecsGameInstanceSubsystem.h"

namespace
{
TAutoConsoleVariable<int32> CVarFlecsOverlay(TEXT("flecs.Diagnostics.Overlay"), 0,
	TEXT("Show the per-world Flecs diagnostics overlay (0 off, 1 on)."), ECVF_Default);
}

void UFlecsNetworkDiagnosticsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LastSampleTime = FPlatformTime::Seconds();
	if (GetWorld() && GetWorld()->GetNetMode() != NM_DedicatedServer)
		DrawHandle = UDebugDrawService::Register(TEXT("OnScreenDebug"),
			FDebugDrawDelegate::CreateUObject(this, &UFlecsNetworkDiagnosticsSubsystem::DrawOverlay));
}
void UFlecsNetworkDiagnosticsSubsystem::Deinitialize()
{
	if (DrawHandle.IsValid())
		UDebugDrawService::Unregister(DrawHandle);
	DEC_MEMORY_STAT_BY(STAT_FlecsNetworkBuffers, Snapshot.NetworkBufferBytes);
	Snapshot = {};
	Super::Deinitialize();
}
bool UFlecsNetworkDiagnosticsSubsystem::IsTickable() const
{
	return !IsTemplate() && GetWorld() != nullptr;
}
TStatId UFlecsNetworkDiagnosticsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFlecsNetworkDiagnosticsSubsystem, STATGROUP_Tickables);
}
void UFlecsNetworkDiagnosticsSubsystem::RecordPayload(int32 Kind, bool bSent, uint64 Bytes)
{
	check(IsInGameThread());
	if (Kind < 0 || Kind >= 3)
		return;
	uint64& Total = bSent ? Snapshot.Sent[Kind] : Snapshot.Received[Kind];
	Total = MAX_uint64 - Total < Bytes ? MAX_uint64 : Total + Bytes;
	const uint32 StatBytes = static_cast<uint32>(FMath::Min<uint64>(Bytes, MAX_uint32));
	if (Kind == 0)
	{
		if (bSent)
		{
			INC_DWORD_STAT_BY(STAT_FlecsNetworkStateSent, StatBytes);
		}
		else
		{
			INC_DWORD_STAT_BY(STAT_FlecsNetworkStateReceived, StatBytes);
		}
	}
	else if (Kind == 1)
	{
		if (bSent)
		{
			INC_DWORD_STAT_BY(STAT_FlecsNetworkCommandSent, StatBytes);
		}
		else
		{
			INC_DWORD_STAT_BY(STAT_FlecsNetworkCommandReceived, StatBytes);
		}
	}
	else
	{
		if (bSent)
		{
			INC_DWORD_STAT_BY(STAT_FlecsNetworkManifestSent, StatBytes);
		}
		else
		{
			INC_DWORD_STAT_BY(STAT_FlecsNetworkManifestReceived, StatBytes);
		}
	}
}
void UFlecsNetworkDiagnosticsSubsystem::ResetRelevance()
{
	Snapshot.Relevant = 0;
	Snapshot.Selected = 0;
	Snapshot.Deferred = 0;
}
void UFlecsNetworkDiagnosticsSubsystem::AddRelevance(uint32 Relevant, uint32 Selected, uint32 Deferred)
{
	Snapshot.Relevant += Relevant;
	Snapshot.Selected += Selected;
	Snapshot.Deferred += Deferred;
	INC_DWORD_STAT_BY(STAT_FlecsNetworkRelevant, Relevant);
	INC_DWORD_STAT_BY(STAT_FlecsNetworkSelected, Selected);
	INC_DWORD_STAT_BY(STAT_FlecsNetworkDeferred, Deferred);
}
void UFlecsNetworkDiagnosticsSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	const double Now = FPlatformTime::Seconds();
	const double Elapsed = Now - LastSampleTime;
	if (Elapsed < 1.0)
		return;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Snapshot.SentBytesPerSecond[Index] = (Snapshot.Sent[Index] - PreviousSent[Index]) / Elapsed;
		Snapshot.ReceivedBytesPerSecond[Index] = (Snapshot.Received[Index] - PreviousReceived[Index]) / Elapsed;
		PreviousSent[Index] = Snapshot.Sent[Index];
		PreviousReceived[Index] = Snapshot.Received[Index];
	}
	uint64 BufferBytes = 0;
	if (UWorld* World = GetWorld())
		for (TActorIterator<AFlecsReplicationChannel> It(World); It; ++It)
			BufferBytes += It->GetDiagnosticsAllocatedBytes();
	if (BufferBytes >= Snapshot.NetworkBufferBytes)
	{
		INC_MEMORY_STAT_BY(STAT_FlecsNetworkBuffers, BufferBytes - Snapshot.NetworkBufferBytes);
	}
	else
	{
		DEC_MEMORY_STAT_BY(STAT_FlecsNetworkBuffers, Snapshot.NetworkBufferBytes - BufferBytes);
	}
	Snapshot.NetworkBufferBytes = BufferBytes;
	LastSampleTime = Now;
}
void UFlecsNetworkDiagnosticsSubsystem::DrawOverlay(UCanvas* Canvas, APlayerController* Controller)
{
	if (CVarFlecsOverlay.GetValueOnGameThread() == 0 || !Canvas || !Controller || !Controller->IsLocalController()
		|| Controller->GetWorld() != GetWorld() || !GEngine || !GEngine->GetSmallFont())
		return;
	const UGameInstance* Game = GetWorld()->GetGameInstance();
	const UFlecsGameInstanceSubsystem* Flecs = Game ? Game->GetSubsystem<UFlecsGameInstanceSubsystem>() : nullptr;
	const auto Publication = Flecs && Flecs->IsWorldReady(*GetWorld())
							   ? Flecs->GetPublication()
							   : TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe>();
	FString Text = TEXT("Flecs (logical payload; not wire bandwidth)\n");
	if (Publication.IsValid())
	{
		Text +=
			FString::Printf(TEXT("Tick %llu | step %.3f ms | queue %d | missed %llu\nPublication allocated %llu B | "),
				Publication->Tick, Publication->LastStepSeconds * 1000.0, Publication->QueueDepth,
				Publication->MissedDeadlines, Publication->PublicationAllocatedBytes);
		Text += Publication->bSystemTimeAvailable
				  ? FString::Printf(TEXT("systems %.3f ms | "), Publication->LastSystemSeconds * 1000.0)
				  : TEXT("system timing: disabled | ");
		Text += Publication->bWorldMemoryAvailable
				  ? FString::Printf(TEXT("Flecs world %llu B\n"), Publication->WorldMemoryBytes)
				  : TEXT("Flecs world memory: disabled\n");
	}
	Text += FString::Printf(TEXT("Totals state %llu/%llu, command %llu/%llu, manifest %llu/%llu B (sent/received)\n"),
		Snapshot.Sent[0], Snapshot.Received[0], Snapshot.Sent[1], Snapshot.Received[1], Snapshot.Sent[2],
		Snapshot.Received[2]);
	Text += FString::Printf(
		TEXT(
			"Rates state %.0f/%.0f, command %.0f/%.0f, manifest %.0f/%.0f B/s (sent/received)\nBuffers %llu B | relevant %u | selected %u | deferred %u"),
		Snapshot.SentBytesPerSecond[0], Snapshot.ReceivedBytesPerSecond[0], Snapshot.SentBytesPerSecond[1],
		Snapshot.ReceivedBytesPerSecond[1], Snapshot.SentBytesPerSecond[2], Snapshot.ReceivedBytesPerSecond[2],
		Snapshot.NetworkBufferBytes, Snapshot.Relevant, Snapshot.Selected, Snapshot.Deferred);
	Canvas->DrawText(GEngine->GetSmallFont(), Text, 24.0f, 24.0f);
}
