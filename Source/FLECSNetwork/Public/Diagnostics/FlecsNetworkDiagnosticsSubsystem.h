#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Diagnostics/FlecsNetworkDiagnosticsSnapshot.h"
#include "FlecsNetworkDiagnosticsSubsystem.generated.h"

class UCanvas;
class APlayerController;

/** Game-thread diagnostics for one world. Payloads are logical bytes, not wire bandwidth. */
UCLASS()
class FLECSNETWORK_API UFlecsNetworkDiagnosticsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	void RecordPayload(int32 Kind, bool bSent, uint64 Bytes);
	void ResetRelevance();
	void AddRelevance(uint32 Relevant, uint32 Selected, uint32 Deferred);
	FFlecsNetworkDiagnosticsSnapshot GetSnapshot() const { return Snapshot; }

private:
	void DrawOverlay(UCanvas* Canvas, APlayerController* Controller);
	FFlecsNetworkDiagnosticsSnapshot Snapshot;
	uint64 PreviousSent[3] = {};
	uint64 PreviousReceived[3] = {};
	double LastSampleTime = 0.0;
	FDelegateHandle DrawHandle;
};
