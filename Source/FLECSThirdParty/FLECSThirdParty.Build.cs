// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class FLECSThirdParty : ModuleRules
{
	public FLECSThirdParty(ReadOnlyTargetRules Target)
		: base(Target)
	{
		Type = ModuleType.External;

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string FlecsLibraryPath =
				Path.Combine(ModuleDirectory, "..", "ThirdParty", "flecs", "Lib", "Win64", "flecs_static.lib");
			PublicAdditionalLibraries.Add(FlecsLibraryPath);
		}
	}
}
