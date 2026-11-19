// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class FLECS : ModuleRules
{
	public FLECS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;


		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
			}
			);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("FLECS_NDEBUG");
		}
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Link Flecs into this module only; consumers import its curated exports.
				"FLECSThirdParty",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
				"Slate",
				"SlateCore",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
