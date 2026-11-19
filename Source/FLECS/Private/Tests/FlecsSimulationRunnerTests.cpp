#if WITH_DEV_AUTOMATION_TESTS

#include "FlecsSimulationRunner.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "Misc/AutomationTest.h"

#include <atomic>

namespace
{
	template <typename Predicate>
	bool WaitUntil(Predicate&& Condition, double TimeoutSeconds = 2.0)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Condition())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
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
	if (!TestTrue(TEXT("Worker starts"), Runner.Start())) return false;
	TestTrue(TEXT("System registration is accepted"), Runner.Submit([&](flecs::world& World)
	{
		World.system<>("ClockTest").run([&](flecs::iter& Iter)
		{
			CallbackThread.store(FPlatformTLS::GetCurrentThreadId());
			if (FMath::Abs(static_cast<double>(Iter.delta_time()) - FFlecsSimulationRunner::FixedStepSeconds) > 0.000001)
			{
				bWrongDelta.store(true);
			}
			++SampleCount;
		});
	}));
	TestTrue(TEXT("Worker progresses while game thread waits"), WaitUntil([&] { return SampleCount.load() >= 3; }));
	TestFalse(TEXT("Every observed simulation delta is fixed"), bWrongDelta.load());
	TestTrue(TEXT("System runs on a different thread"), CallbackThread.load() != 0 && CallbackThread.load() != TestThread);
	Runner.SetPaused(true);
	// Wait for the worker to publish its paused state, including any in-flight step.
	TestTrue(TEXT("Paused state is published"), WaitUntil([&]
	{
		auto Publication = Runner.GetPublication();
		return Publication.IsValid() && Publication->LastStepSeconds == 0.0;
	}));
	const uint32 PausedSamples = SampleCount.load();
	std::atomic<bool> bPausedCommandApplied{false};
	TestTrue(TEXT("Paused worker accepts queued work"), Runner.Submit([&](flecs::world&) { bPausedCommandApplied.store(true); }));
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
	if (!TestTrue(TEXT("Worker starts paused"), Runner.Start())) return false;
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

#endif
