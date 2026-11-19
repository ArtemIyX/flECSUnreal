using UnrealBuildTool;

public class FLECSModding : ModuleRules
{
	public FLECSModding(ReadOnlyTargetRules Target)
		: base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.Add("Core");
	}
}
