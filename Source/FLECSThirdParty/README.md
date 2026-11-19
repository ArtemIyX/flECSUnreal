# FLECSThirdParty

The Unreal Build Tool external module that links the bundled Flecs static library into the `FLECS` runtime module. It is a build dependency, not a runtime module listed in `FLECS.uplugin`; game modules should normally depend on `FLECS`, not link the static library again.

## Layout and support

- Headers: `Source/ThirdParty/flecs/Include`.
- Configured static library: `Source/ThirdParty/flecs/Lib/Win64/flecs_static.lib`.
- `FLECSThirdParty.Build.cs` currently configures the library only for Win64. Do not assume other platforms have a bundled library or link configuration.

The `FLECS` module links Flecs once. Other modules import the curated native symbol list declared in `FLECS/Private/FLECS.cpp`; that list is not a complete export of the upstream API. If an inline Flecs C++ call from another module needs a new native symbol, update the curated export list deliberately and check the target platform's linking behavior. Avoid adding a second static Flecs copy to a consuming module.

Keep the bundled headers and static library built from compatible Flecs versions and build options. Changes to third-party artifacts, exports, or platform support require their own validation; the Unreal plugin descriptor's Iris dependency is configured separately.
