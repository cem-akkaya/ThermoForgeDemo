// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ThermoForgeDemo : ModuleRules
{
	public ThermoForgeDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"ThermoForgeDemo",
			"ThermoForgeDemo/Variant_Platforming",
			"ThermoForgeDemo/Variant_Platforming/Animation",
			"ThermoForgeDemo/Variant_Combat",
			"ThermoForgeDemo/Variant_Combat/AI",
			"ThermoForgeDemo/Variant_Combat/Animation",
			"ThermoForgeDemo/Variant_Combat/Gameplay",
			"ThermoForgeDemo/Variant_Combat/Interfaces",
			"ThermoForgeDemo/Variant_Combat/UI",
			"ThermoForgeDemo/Variant_SideScrolling",
			"ThermoForgeDemo/Variant_SideScrolling/AI",
			"ThermoForgeDemo/Variant_SideScrolling/Gameplay",
			"ThermoForgeDemo/Variant_SideScrolling/Interfaces",
			"ThermoForgeDemo/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
