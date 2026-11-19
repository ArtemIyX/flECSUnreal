#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Relevance/FlecsRelevanceScheduler.h"

/** Reproducible scheduler-only measurements; this does not measure Iris or wire bytes. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRelevanceProfile, "Flecs.Network.Profile.Scheduler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRelevanceProfile::RunTest(const FString& Parameters)
{
	(void)Parameters;
	constexpr int32 Frames = 60;
	constexpr int32 Clients = 4;
	constexpr float CellSize = 1000.0f;
	const int32 Counts[] = {100, 500, 5000};
	AddInfo(TEXT(
		"Scheduler-only profile: 60 synthetic 60 Hz steps, four client views, 128 records/8192 estimated bytes per client per step. No Iris, socket, packet-loss, or wire-byte measurement."));
	for (const int32 EntityCount : Counts)
	{
		FFlecsSpatialSnapshot Snapshot;
		Snapshot.WorldEpoch = 1;
		Snapshot.Entities.Reserve(EntityCount);
		for (int32 Index = 0; Index < EntityCount; ++Index)
		{
			FFlecsSpatialEntity& Entity = Snapshot.Entities.AddDefaulted_GetRef();
			Entity.Key = {1, static_cast<uint32>(Index + 1), 1};
			Entity.Position =
				FVector3f(static_cast<float>((Index % 100) * 200), static_cast<float>((Index / 100) * 200), 0.0f);
			Entity.Revision = 1;
			Entity.EstimatedUpdateBytes = 48;
		}
		TArray<FFlecsPerConnectionScheduler> Schedulers;
		Schedulers.SetNum(Clients);
		TArray<FFlecsConnectionInterest> Interests;
		for (int32 Client = 0; Client < Clients; ++Client)
		{
			FFlecsConnectionInterest& Interest = Interests.AddDefaulted_GetRef();
			Interest.ConnectionId = static_cast<uint64>(Client + 1);
			Interest.ViewPosition =
				FVector3f(static_cast<float>(Client * 5000), static_cast<float>((Client % 2) * 4000), 0.0f);
			Interest.Tiers = {{2000, 30}, {5000, 15}, {10000, 7.5f}, {20000, 3.75f}};
			Interest.HysteresisDistance = 200;
		}
		FFlecsRelevanceBudget Budget;
		Budget.MaxRecords = 128;
		Budget.MaxBytes = 8192;
		FFlecsSpatialIndex SpatialIndex;
		double IndexSeconds = 0.0;
		double SelectionSeconds = 0.0;
		uint64 Selected = 0, EstimatedBytes = 0, Deferred = 0, MaxDeferredAgeMs = 0;
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			Snapshot.SimulationTick = Frame + 1;
			for (FFlecsSpatialEntity& Entity : Snapshot.Entities)
				Entity.Revision = Snapshot.SimulationTick;
			const double IndexStart = FPlatformTime::Seconds();
			const bool bIndexed = SpatialIndex.Build(Snapshot, CellSize);
			IndexSeconds += FPlatformTime::Seconds() - IndexStart;
			if (!bIndexed)
			{
				AddError(FString::Printf(TEXT("Index build failed for %d entities at step %d."), EntityCount, Frame));
				return false;
			}
			const double Now = 1.0 + static_cast<double>(Frame) / 60.0;
			for (int32 Client = 0; Client < Clients; ++Client)
			{
				FFlecsRelevanceSelection Selection;
				const double SelectionStart = FPlatformTime::Seconds();
				const bool bSelected =
					Schedulers[Client].Select(SpatialIndex, Interests[Client], Now, Budget, Selection);
				SelectionSeconds += FPlatformTime::Seconds() - SelectionStart;
				if (!bSelected
					|| (Selection.SelectionId != 0
						&& !Schedulers[Client].CompleteSelection(Selection.SelectionId, true, Now)))
				{
					AddError(FString::Printf(TEXT("Selection/handoff failed for %d entities, client %d, step %d."),
						EntityCount, Client, Frame));
					return false;
				}
				Selected += Selection.Metrics.SelectedRecords;
				EstimatedBytes += Selection.Metrics.SelectedBytes;
				Deferred += Selection.Metrics.DeferredByBudget;
				MaxDeferredAgeMs = FMath::Max(MaxDeferredAgeMs, Selection.Metrics.OldestDeferredAgeMs);
			}
		}
		AddInfo(FString::Printf(
			TEXT(
				"entities=%d steps=%d clients=%d index_total_ms=%.3f index_mean_ms=%.3f selection_total_ms=%.3f selection_mean_ms=%.3f selected=%llu estimated_selected_bytes=%llu deferred=%llu max_deferred_age_ms=%llu"),
			EntityCount, Frames, Clients, IndexSeconds * 1000.0, IndexSeconds * 1000.0 / Frames,
			SelectionSeconds * 1000.0, SelectionSeconds * 1000.0 / (Frames * Clients), Selected, EstimatedBytes,
			Deferred, MaxDeferredAgeMs));
	}
	return true;
}

#endif
