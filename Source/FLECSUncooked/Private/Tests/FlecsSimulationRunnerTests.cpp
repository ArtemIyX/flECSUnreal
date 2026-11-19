#if WITH_DEV_AUTOMATION_TESTS

#include "FlecsSimulationRunner.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "Misc/AutomationTest.h"

#include <atomic>

namespace
{
template <typename Predicate> bool WaitUntil(Predicate&& Condition, double TimeoutSeconds = 2.0)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!Condition())
	{
		if (FPlatformTime::Seconds() >= Deadline)
			return false;
		FPlatformProcess::SleepNoStats(0.002f);
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRunnerClockTest, "Flecs.Simulation.WorkerClockAndPause",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRunnerClockTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	std::atomic<uint32> SampleCount{0};
	std::atomic<bool> bWrongDelta{false};
	std::atomic<uint32> CallbackThread{0};
	const uint32 TestThread = FPlatformTLS::GetCurrentThreadId();
	FFlecsSimulationRunner Runner(42);
	if (!TestTrue(TEXT("Worker starts"), Runner.Start()))
		return false;
	TestTrue(TEXT("System registration is accepted"), Runner.Submit([&](flecs::world& World) {
		World.system<>("ClockTest").run([&](flecs::iter& Iter) {
			CallbackThread.store(FPlatformTLS::GetCurrentThreadId());
			if (FMath::Abs(static_cast<double>(Iter.delta_time()) - FFlecsSimulationRunner::FixedStepSeconds)
				> 0.000001)
			{
				bWrongDelta.store(true);
			}
			++SampleCount;
		});
	}));
	TestTrue(TEXT("Worker progresses while game thread waits"), WaitUntil([&] { return SampleCount.load() >= 3; }));
	TestFalse(TEXT("Every observed simulation delta is fixed"), bWrongDelta.load());
	TestTrue(
		TEXT("System runs on a different thread"), CallbackThread.load() != 0 && CallbackThread.load() != TestThread);
	Runner.SetPaused(true);
	// Wait for the worker to publish its paused state, including any in-flight step.
	TestTrue(TEXT("Paused state is published"), WaitUntil([&] {
		auto Publication = Runner.GetPublication();
		return Publication.IsValid() && Publication->LastStepSeconds == 0.0;
	}));
	const uint32 PausedSamples = SampleCount.load();
	std::atomic<bool> bPausedCommandApplied{false};
	TestTrue(TEXT("Paused worker accepts queued work"),
		Runner.Submit([&](flecs::world&) { bPausedCommandApplied.store(true); }));
	FPlatformProcess::SleepNoStats(0.08f);
	TestEqual(TEXT("Paused worker does not advance"), SampleCount.load(), PausedSamples);
	TestFalse(TEXT("Queued work waits while paused"), bPausedCommandApplied.load());
	Runner.SetPaused(false);
	TestTrue(TEXT("Resume advances the worker"), WaitUntil([&] { return SampleCount.load() > PausedSamples; }));
	TestTrue(TEXT("Resume applies queued work"), WaitUntil([&] { return bPausedCommandApplied.load(); }));
	auto Publication = Runner.GetPublication();
	TestTrue(TEXT("Publication identifies session epoch"), Publication.IsValid() && Publication->Epoch == 42);
	Runner.StopAndJoin();
	const uint32 StoppedSamples = SampleCount.load();
	FPlatformProcess::SleepNoStats(0.04f);
	TestEqual(TEXT("No callback runs after join"), SampleCount.load(), StoppedSamples);
	TestFalse(TEXT("Stopped worker rejects new work"), Runner.Submit([](flecs::world&) {}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRunnerQueueTest, "Flecs.Simulation.BoundedPausedQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRunnerQueueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	std::atomic<int32> Applied{0};
	FFlecsSimulationRunner Runner(43);
	Runner.SetPaused(true);
	if (!TestTrue(TEXT("Worker starts paused"), Runner.Start()))
		return false;
	for (uint64 Index = 0; Index < FFlecsSimulationRunner::MaxQueuedCommands; ++Index)
	{
		if (!Runner.Submit([&](flecs::world&) { ++Applied; }))
		{
			AddError(TEXT("Queue rejected work before reaching its advertised bound."));
			break;
		}
	}
	TestFalse(TEXT("Full queue rejects additional work"), Runner.Submit([](flecs::world&) {}));
	TestEqual(TEXT("Paused work is not applied"), Applied.load(), 0);
	Runner.StopAndJoin();
	TestEqual(TEXT("Shutdown cancels queued work"), Applied.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRunnerStartupPublicationTest, "Flecs.Simulation.StartupAndPublication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRunnerStartupPublicationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	std::atomic<uint32> StepsObserved{0};
	std::atomic<uint32> StartupThread{0};
	std::atomic<bool> bMissingFirstStep{false};
	const uint32 CallerThread = FPlatformTLS::GetCurrentThreadId();
	TArray<FFlecsWorkerCommand> Startup;
	Startup.Add([&](flecs::world& World) {
		StartupThread.store(FPlatformTLS::GetCurrentThreadId());
		World.system<>("StartupTest").run([&](flecs::iter&) { ++StepsObserved; });
	});
	TArray<FFlecsWorkerDataPublisher> Publishers;
	Publishers.Add([&](flecs::world&, FFlecsSimulationDataBuilder& Builder) {
		const uint32 Step = StepsObserved.load();
		if (Step == 0)
			bMissingFirstStep.store(true);
		uint8 Bytes[] = {static_cast<uint8>(Step & 0xff), 42};
		Builder.TryAdd(17, MakeArrayView(Bytes));
		Bytes[1] = 0; // Publication must own a copy, not alias the caller's storage.
		Builder.TryAdd(0, MakeArrayView(Bytes));
	});
	FFlecsSimulationRunner Runner(91);
	if (!TestTrue(TEXT("Start with startup registration"), Runner.Start(MoveTemp(Startup), MoveTemp(Publishers))))
		return false;
	TestTrue(TEXT("Registration completes before Start returns"), StartupThread.load() != 0);
	TestTrue(TEXT("Registration runs on worker"), StartupThread.load() != CallerThread);
	TestTrue(TEXT("Data publication appears"), WaitUntil([&] { return Runner.GetDataPublication().IsValid(); }));
	auto First = Runner.GetDataPublication();
	if (!TestTrue(TEXT("Data publication exists"), First.IsValid()))
	{
		Runner.StopAndJoin();
		return false;
	}
	TestFalse(TEXT("Startup system runs before first extraction"), bMissingFirstStep.load());
	TestEqual(TEXT("Publication carries local attachment epoch"), First->LocalAttachmentEpoch, uint64(91));
	TestTrue(TEXT("Publication has a simulation tick"), First->SimulationTick > 0);
	TestEqual(TEXT("Accepted item count"), First->Items.Num(), 1);
	TestEqual(TEXT("Invalid item counted"), First->RejectedItems, uint32(1));
	TestEqual(TEXT("Byte count"), First->TotalBytes, uint32(2));
	if (First->Items.Num() == 1 && First->Items[0].Payload.Num() == 2)
	{
		TestEqual(TEXT("Payload copied from caller buffer"), First->Items[0].Payload[1], uint8(42));
	}
	else
		AddError(TEXT("Expected one two-byte payload."));
	const uint64 FirstTick = First->SimulationTick;
	const uint8 FirstValue = First->Items.Num() && First->Items[0].Payload.Num() ? First->Items[0].Payload[0] : 0;
	TestTrue(TEXT("A later publication appears"), WaitUntil([&] {
		auto Latest = Runner.GetDataPublication();
		return Latest.IsValid() && Latest->SimulationTick > FirstTick;
	}));
	TestEqual(TEXT("Held publication retains its original tick"), First->SimulationTick, FirstTick);
	if (First->Items.Num() && First->Items[0].Payload.Num())
	{
		TestEqual(TEXT("Held publication bytes remain unchanged"), First->Items[0].Payload[0], FirstValue);
	}
	Runner.StopAndJoin();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsPublicationBoundsTest, "Flecs.Simulation.PublicationBoundsAndEpoch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsPublicationBoundsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSimulationDataBuilder Builder;
	TArray<uint8> TooLarge;
	TooLarge.SetNumZeroed(FlecsMaxDataPublicationItemBytes + 1);
	TestTrue(
		TEXT("Oversized item rejected"), Builder.TryAdd(1, TooLarge) == EFlecsDataPublicationAddResult::ItemTooLarge);
	TArray<uint8> Chunk;
	Chunk.SetNumZeroed(FlecsMaxDataPublicationItemBytes);
	for (uint32 Index = 0; Index < FlecsMaxDataPublicationBytes / FlecsMaxDataPublicationItemBytes; ++Index)
	{
		if (Builder.TryAdd(1, Chunk) != EFlecsDataPublicationAddResult::Accepted)
		{
			AddError(TEXT("Publication rejected bytes before its declared cap."));
			break;
		}
	}
	const uint8 One = 1;
	TestTrue(TEXT("Total byte cap rejects overflow"),
		Builder.TryAdd(1, MakeArrayView(&One, 1)) == EFlecsDataPublicationAddResult::ByteLimitReached);
	TestEqual(TEXT("Byte cap is not exceeded"), Builder.GetTotalBytes(), FlecsMaxDataPublicationBytes);
	FFlecsSimulationDataBuilder ItemBuilder;
	for (uint32 Index = 0; Index < FlecsMaxDataPublicationItems; ++Index)
	{
		if (ItemBuilder.TryAdd(1, TConstArrayView<uint8>()) != EFlecsDataPublicationAddResult::Accepted)
		{
			AddError(TEXT("Publication rejected items before its declared cap."));
			break;
		}
	}
	TestTrue(TEXT("Item cap rejects overflow"),
		ItemBuilder.TryAdd(1, TConstArrayView<uint8>()) == EFlecsDataPublicationAddResult::ItemLimitReached);

	std::atomic<uint32> Executions{0};
	FFlecsSimulationRunner Runner(92);
	Runner.SetPaused(true);
	if (!TestTrue(TEXT("Start paused session"), Runner.Start()))
		return false;
	TestTrue(TEXT("Old local epoch is rejected"),
		Runner.Submit(91, [&](flecs::world&) { ++Executions; }) == EFlecsWorkerCommandResult::StaleEpoch);
	TestTrue(TEXT("Current local epoch is accepted"),
		Runner.Submit(92, [&](flecs::world&) { ++Executions; }) == EFlecsWorkerCommandResult::Accepted);
	Runner.SetPaused(false);
	TestTrue(TEXT("Current epoch work runs"), WaitUntil([&] { return Executions.load() > 0; }));
	Runner.StopAndJoin();
	TestEqual(TEXT("Stale command never executes"), Executions.load(), uint32(1));
	return true;
}

#endif
