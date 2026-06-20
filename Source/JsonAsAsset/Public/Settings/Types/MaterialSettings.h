/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MaterialSettings.generated.h"

UENUM()
enum class EMaterialFallbackMode : uint8 {
	None,
	LegacyStubs,
	Approximation,
	ApproximationThenLegacyStubs
};

/* Settings for materials */
USTRUCT()
struct FJMaterialSettings {
	GENERATED_BODY()
public:
	/**
	 * Prevents a known error: "Material expression called Compiler->TextureParameter() without implementing UMaterialExpression::GetReferencedTexture properly."
	 *
	 * To avoid this issue, this option skips connecting the inputs to the material's primary result node, potentially fixing the error.
	 *
	 * Usage:
	 *  - If enabled, import the material, save your project, restart the editor, and then re-import the material.
	 *  - Alternatively, manually connect the inputs to the main result node.
	 */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool DisconnectRoot = false;

	/* Creates stub versions of materials that have parameters (for Modding) */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool Stubs = false;

	/* Controls what JsonAsAsset creates when editor-only material expressions are missing. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	EMaterialFallbackMode FallbackMode = EMaterialFallbackMode::ApproximationThenLegacyStubs;

	/* Uses sibling material instances from folder import to expose matching parent parameters. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool UseSiblingMaterialInstancesForApproximation = true;

	/* Creates parameter nodes for unclassified material instance parameters without wiring them into outputs. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool CreateUnconnectedUnknownParameterNodes = true;

	/* Prefer exact material instance parameter names over generic generated names. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool PreferMaterialInstanceParameterNames = true;

	/* Generate generic parameter names from parent material texture data when no child instances are available. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool GenerateGenericParametersWhenNoInstancesFound = true;
};
