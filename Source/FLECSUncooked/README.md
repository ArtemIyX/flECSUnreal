# FLECSUncooked

An `UncookedOnly` plugin module containing Unreal automation coverage for the reusable Flecs modules. It is not a runtime or cooked-game dependency. The module depends on `FLECS`, `FLECSModding`, `FLECSNetwork`, Unreal networking, and Iris support; its tests can include the core module's private runner header.

## Coverage

`Private/Tests` contains cases for worker lifecycle and bounded publications; entity-model and spatial/relevance contracts; schema and manifest compatibility; channel clock, Iris bounded payloads, and quantization; world modes; and diagnostics. Automation tests are compiled under `WITH_DEV_AUTOMATION_TESTS`. The native example-mod and example movement tests remain in the host game project because they depend on game code.

Find tests in the Unreal Automation window under `Flecs`, or use the Unreal automation console command `Automation RunTests Flecs` in an appropriate editor session. Run a narrower named test while investigating a failure. Some tests create worlds or workers, so use a suitable editor/test environment and clean up the session afterwards.

The presence of tests is not a statement that a particular platform, configuration, dedicated server, or live network session has passed. Builds, automation, live-session verification, and profiling are separate activities.
