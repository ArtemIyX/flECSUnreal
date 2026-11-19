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
	bool WaitForLifecycle(Predicate&& Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + 2.0;
		while (!Condition())
		{
			if (FPlatformTime::Seconds() >= Deadline) return false;
			FPlatformProcess::SleepNoStats(0.002f);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsLateLifecycleTest, "Flecs.Simulation.LateLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsLateLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	std::atomic<uint32> Starts{0}, Stops{0}, PublisherCalls{0}, WorkerThread{0};
	std::atomic<bool> bEpochValid{true}, bWorldAliveAtShutdown{false};
	FFlecsSimulationRunner Runner(101);
	Runner.SetPaused(true);
	if (!TestTrue(TEXT("Runner starts paused"), Runner.Start())) return false;
	const uint32 CallerThread = FPlatformTLS::GetCurrentThreadId();
	auto Startup = [&](flecs::world& World, uint64 Epoch)
	{
		WorkerThread.store(FPlatformTLS::GetCurrentThreadId());
		if (Epoch != 101) bEpochValid.store(false);
		World.entity("LifecycleTestEntity");
		++Starts;
	};
	auto Shutdown = [&](flecs::world& World, uint64 Epoch)
	{
		if (Epoch != 101) bEpochValid.store(false);
		auto Entity = World.lookup("LifecycleTestEntity");
		bWorldAliveAtShutdown.store(Entity.is_alive());
		if (Entity.is_alive()) Entity.destruct();
		++Stops;
	};
	TestTrue(TEXT("Stale lifecycle registration rejected"),
		Runner.RegisterLifecycle(100, TEXT("test"), Startup, Shutdown) == EFlecsWorkerCommandResult::StaleEpoch);
	TestTrue(TEXT("Late lifecycle accepted"),
		Runner.RegisterLifecycle(101, TEXT("test"), Startup, Shutdown) == EFlecsWorkerCommandResult::Accepted);
	TestTrue(TEXT("Late publisher accepted"), Runner.RegisterPublisher(101, TEXT("publisher"),
		[&](flecs::world&, FFlecsSimulationDataBuilder&) { ++PublisherCalls; }) == EFlecsWorkerCommandResult::Accepted);
	FPlatformProcess::SleepNoStats(0.06f);
	TestEqual(TEXT("Paused controls wait for a step boundary"), Starts.load(), uint32(0));
	TestEqual(TEXT("Paused publisher does not run"), PublisherCalls.load(), uint32(0));
	Runner.SetPaused(false);
	TestTrue(TEXT("Lifecycle starts and publisher runs after resume"),
		WaitForLifecycle([&] { return Starts.load() == 1 && PublisherCalls.load() > 0; }));
	TestTrue(TEXT("Startup runs on worker thread"), WorkerThread.load() != 0 && WorkerThread.load() != CallerThread);
	TestTrue(TEXT("Lifecycle receives current epoch"), bEpochValid.load());
	TestTrue(TEXT("Stale removal rejected"), Runner.UnregisterLifecycle(100, TEXT("test")) == EFlecsWorkerCommandResult::StaleEpoch);
	TestTrue(TEXT("Removal accepted"), Runner.UnregisterLifecycle(101, TEXT("test")) == EFlecsWorkerCommandResult::Accepted);
	TestTrue(TEXT("Unregister runs paired shutdown"), WaitForLifecycle([&] { return Stops.load() == 1; }));
	TestTrue(TEXT("Shutdown can resolve live Flecs entity"), bWorldAliveAtShutdown.load());
	TestTrue(TEXT("Publisher removal accepted"), Runner.UnregisterPublisher(101, TEXT("publisher")) == EFlecsWorkerCommandResult::Accepted);
	Runner.StopAndJoin();
	Runner.StopAndJoin();
	TestEqual(TEXT("Unregistered lifecycle shuts down only once"), Stops.load(), uint32(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsLifecycleStopDrainTest, "Flecs.Simulation.LifecycleStopDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsLifecycleStopDrainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	std::atomic<uint32> Starts{0}, Stops{0}, Commands{0};
	std::atomic<bool> bWorldAlive{true};
	FFlecsSimulationRunner Runner(102);
	Runner.SetPaused(true);
	if (!TestTrue(TEXT("Runner starts paused"), Runner.Start())) return false;
	auto Startup = [&](flecs::world& World, uint64) { World.entity("DrainEntity"); ++Starts; };
	auto Shutdown = [&](flecs::world& World, uint64)
	{
		auto Entity = World.lookup("DrainEntity");
		if (!Entity.is_alive()) bWorldAlive.store(false);
		else Entity.destruct();
		++Stops;
	};
	TestTrue(TEXT("Lifecycle add accepted"), Runner.RegisterLifecycle(102, TEXT("a"), Startup, Shutdown) == EFlecsWorkerCommandResult::Accepted);
	TestTrue(TEXT("Lifecycle remove accepted"), Runner.UnregisterLifecycle(102, TEXT("a")) == EFlecsWorkerCommandResult::Accepted);
	TestTrue(TEXT("Second lifecycle accepted"), Runner.RegisterLifecycle(102, TEXT("b"), Startup, Shutdown) == EFlecsWorkerCommandResult::Accepted);
	TestTrue(TEXT("Ordinary command accepted"), Runner.Submit(102, [&](flecs::world&) { ++Commands; }) == EFlecsWorkerCommandResult::Accepted);
	Runner.StopAndJoin();
	TestEqual(TEXT("Accepted adds resolved during stop"), Starts.load(), uint32(2));
	TestEqual(TEXT("Accepted removal and final teardown each run once"), Stops.load(), uint32(2));
	TestEqual(TEXT("Ordinary commands are cancelled at stop"), Commands.load(), uint32(0));
	TestTrue(TEXT("Both shutdown callbacks run while world is alive"), bWorldAlive.load());
	TestTrue(TEXT("Controls rejected after stop"), Runner.UnregisterLifecycle(102, TEXT("b")) == EFlecsWorkerCommandResult::Stopping);

	FFlecsSimulationRunner Full(103);
	Full.SetPaused(true);
	if (!TestTrue(TEXT("Queue test starts paused"), Full.Start())) return false;
	bool bFilled = true;
	for (uint64 Index = 0; Index < FFlecsSimulationRunner::MaxQueuedCommands; ++Index)
	{
		bFilled &= Full.Submit(103, [](flecs::world&) {}) == EFlecsWorkerCommandResult::Accepted;
	}
	TestTrue(TEXT("Queue fills to advertised capacity"), bFilled);
	TestTrue(TEXT("Lifecycle control reports queue full"),
		Full.RegisterLifecycle(103, TEXT("full"), [](flecs::world&, uint64) {}, [](flecs::world&, uint64) {}) == EFlecsWorkerCommandResult::QueueFull);
	Full.StopAndJoin();
	return true;
}

#endif
