/* Copyright JsonAsAsset Contributors 2024-2026 */

#if WITH_DEV_AUTOMATION_TESTS

#include "Importers/Types/Materials/MaterialApproximation.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialApproximationClassifierTest,
	"JsonAsAsset.MaterialApproximation.Classifiers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FMaterialApproximationClassifierTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Base color names classify as base color"),
		FMaterialApproximation::ClassifyTextureRole(TEXT("PM_Diffuse")),
		EApproxTextureRole::BaseColor
	);

	TestEqual(
		TEXT("Packed mask names classify as ORM"),
		FMaterialApproximation::ClassifyTextureRole(TEXT("ORM+Mask")),
		EApproxTextureRole::ORM
	);

	TestEqual(
		TEXT("Blinker emissive names classify before generic emissive"),
		FMaterialApproximation::ClassifyTextureRole(TEXT("Emissive_Blinkers")),
		EApproxTextureRole::EmissiveBlinkers
	);

	TestEqual(
		TEXT("Clear coat roughness scalar is specific"),
		FMaterialApproximation::ClassifyScalarRole(TEXT("Roughness Clear Coat")),
		EApproxScalarRole::ClearCoatRoughness
	);

	TestEqual(
		TEXT("Color vector role is tint"),
		FMaterialApproximation::ClassifyVectorRole(TEXT("Paint Color")),
		EApproxVectorRole::TintColor
	);

	TestEqual(
		TEXT("FModel numeric export suffix is normalized"),
		FMaterialApproximation::NormalizeObjectPath(TEXT("/Game/Foo/T_Bar.0")),
		FString(TEXT("/Game/Foo/T_Bar.T_Bar"))
	);

	return true;
}

#endif
