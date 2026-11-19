using UnrealBuildTool;

public class FLECSNetwork : ModuleRules
{
	public FLECSNetwork(ReadOnlyTargetRules Target)
		: base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"NetCore",
			"FLECSModding",
		});

		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore", "DeveloperSettings" });
		PrivateDependencyModuleNames.Add("FLECS");
		SetupIrisSupport(Target, true);
	}
}
