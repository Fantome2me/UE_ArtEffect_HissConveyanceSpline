// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UE_ConveyanceSpline : ModuleRules
{
	public UE_ConveyanceSpline(ReadOnlyTargetRules Target) : base(Target)
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
			"Slate",
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore", "RHI", "RenderCore" });

		PublicIncludePaths.AddRange(new string[] {
			"UE_ConveyanceSpline",
			"UE_ConveyanceSpline/Variant_Platforming",
			"UE_ConveyanceSpline/Variant_Platforming/Animation",
			"UE_ConveyanceSpline/Variant_Combat",
			"UE_ConveyanceSpline/Variant_Combat/AI",
			"UE_ConveyanceSpline/Variant_Combat/Animation",
			"UE_ConveyanceSpline/Variant_Combat/Gameplay",
			"UE_ConveyanceSpline/Variant_Combat/Interfaces",
			"UE_ConveyanceSpline/Variant_Combat/UI",
			"UE_ConveyanceSpline/Variant_SideScrolling",
			"UE_ConveyanceSpline/Variant_SideScrolling/AI",
			"UE_ConveyanceSpline/Variant_SideScrolling/Gameplay",
			"UE_ConveyanceSpline/Variant_SideScrolling/Interfaces",
			"UE_ConveyanceSpline/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
