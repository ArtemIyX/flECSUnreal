using UnrealBuildTool;

public class FLECSUncooked : ModuleRules
{
	public FLECSUncooked(ReadOnlyTargetRules Target)
		: base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Engine", "FLECS", "FLECSModding",
			"FLECSNetwork", "NetCore", "Slate", "SlateCore", "DeveloperSettings" });
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "FLECS", "Private"));
		SetupIrisSupport(Target, true);
	}
}
