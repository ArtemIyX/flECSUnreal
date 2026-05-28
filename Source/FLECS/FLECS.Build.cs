// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class FLECS : ModuleRules
{
	public FLECS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		string thirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty", "flecs");
		string flecsIncludePath = Path.Combine(thirdPartyPath, "Include");
		string flecsLibPath = Path.Combine(thirdPartyPath, "Lib", "Win64", "flecs_static.lib");
		
		PublicIncludePaths.AddRange(
			new string[] {
				flecsIncludePath
			}
			);
				
		
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
			PublicDefinitions.Add("flecs_STATIC");
			PublicAdditionalLibraries.Add(flecsLibPath);
		}
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
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
