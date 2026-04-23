/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

/* Settings Substructures */
#include "Types/AnimationBlueprintSettings.h"
#include "Types/MaterialSettings.h"
#include "Types/TextureSettings.h"
#include "Redirector.h"

#include "JsonAsAssetSettings.generated.h"

extern FName GJsonAsAssetSettingsCategoryName;
extern FName GJsonAsAssetInternalName;

UENUM()
enum class EJBlueprintReimportPolicy : uint8
{
	ReuseValid UMETA(DisplayName = "ReuseValid"),
	RecreateInvalid UMETA(DisplayName = "RecreateInvalid"),
	AlwaysRecreate UMETA(DisplayName = "AlwaysRecreate")
};

UENUM()
enum class EJBlueprintLogDetail : uint8
{
	Normal UMETA(DisplayName = "Normal"),
	Verbose UMETA(DisplayName = "Verbose")
};

UENUM()
enum class EJBlueprintCompilePolicy : uint8
{
	Immediate UMETA(DisplayName = "Immediate"),
	DeferredBatch UMETA(DisplayName = "DeferredBatch"),
	Manual UMETA(DisplayName = "Manual")
};

UENUM()
enum class EJImportPreset : uint8
{
	Default UMETA(DisplayName = "Default"),
	BetterMartMirror UMETA(DisplayName = "BetterMartMirror")
};

USTRUCT()
struct FJBlueprintImportSettings
{
	GENERATED_BODY()
public:
	/* If true, importer fails when critical references are missing. If false, importer warns and keeps going. */
	UPROPERTY(EditAnywhere, Config, Category = BlueprintImport)
	bool StrictMode = false;

	/* Controls how existing Blueprint/AnimBlueprint assets are handled during reimport. */
	UPROPERTY(EditAnywhere, Config, Category = BlueprintImport)
	EJBlueprintReimportPolicy ReimportPolicy = EJBlueprintReimportPolicy::RecreateInvalid;

	/* Controls importer logging detail for Blueprint/AnimBlueprint import path. */
	UPROPERTY(EditAnywhere, Config, Category = BlueprintImport)
	EJBlueprintLogDetail LogDetail = EJBlueprintLogDetail::Normal;

	/* Controls when Blueprint/AnimBlueprint assets should be compiled during import. */
	UPROPERTY(EditAnywhere, Config, Category = BlueprintImport)
	EJBlueprintCompilePolicy CompilePolicy = EJBlueprintCompilePolicy::DeferredBatch;
};

USTRUCT()
struct FJCompatibilityFallbackSettings
{
	GENERATED_BODY()
public:
	/* Enables compatibility fallback behavior for BlueprintGeneratedClass import edge-cases. */
	UPROPERTY(EditAnywhere, Config, Category = CompatibilityFallback, AdvancedDisplay)
	bool BlueprintGeneratedClass = false;

	/* Enables compatibility fallback behavior for WidgetBlueprintGeneratedClass import edge-cases. */
	UPROPERTY(EditAnywhere, Config, Category = CompatibilityFallback, AdvancedDisplay)
	bool WidgetBlueprintGeneratedClass = false;

	/* Enables compatibility fallback behavior for AnimBlueprintGeneratedClass import edge-cases. */
	UPROPERTY(EditAnywhere, Config, Category = CompatibilityFallback, AdvancedDisplay)
	bool AnimBlueprintGeneratedClass = false;

	/* Enables compatibility fallback behavior for StaticMesh import edge-cases. */
	UPROPERTY(EditAnywhere, Config, Category = CompatibilityFallback, AdvancedDisplay)
	bool StaticMesh = false;

	/* Enables compatibility fallback behavior for MaterialParameterCollection import edge-cases. */
	UPROPERTY(EditAnywhere, Config, Category = CompatibilityFallback, AdvancedDisplay)
	bool MaterialParameterCollection = false;

	/* Enables compatibility fallback behavior for PhysicalMaterial import edge-cases. */
	UPROPERTY(EditAnywhere, Config, Category = CompatibilityFallback, AdvancedDisplay)
	bool PhysicalMaterial = false;
};

USTRUCT()
struct FJSettings
{
	GENERATED_BODY()
public:
	/* Constructor to initialize default values */
	FJSettings() {
		Material = FJMaterialSettings();
		Texture = FJTextureSettings();
		AnimationBlueprint = FJAnimationBlueprintSettings();
	}

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FJAnimationBlueprintSettings AnimationBlueprint;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FJTextureSettings Texture;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FJMaterialSettings Material;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FString ProjectName;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	bool SaveAssets = false;
};

USTRUCT()
struct FJVersioningSettings
{
	GENERATED_BODY()
public:
	/* Disable checking for newer updates of JsonAsAsset. */
	UPROPERTY(EditAnywhere, Config, Category = VersioningSettings)
	bool Disable = false;
};

USTRUCT()
struct FJPresetSettings
{
	GENERATED_BODY()
public:
	/* Optional preset profile for game-specific defaults. */
	UPROPERTY(EditAnywhere, Config, Category = Preset)
	EJImportPreset Profile = EJImportPreset::Default;
};

/* Powerful Unreal Engine Plugin that imports assets from FModel */
UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig)
class JSONASASSET_API UJsonAsAssetSettings : public UDeveloperSettings {
	GENERATED_BODY()
public:
	UJsonAsAssetSettings();

	/* Overriden to stop the Editor spacing the words between JsonAsAsset */
	virtual FText GetSectionText() const override;

	/* Returns true when the BetterMart profile is enabled. */
	bool IsBetterMartPresetEnabled() const;

	/* Applies runtime-only preset behavior. */
	void ApplyPresetRuntimeOverrides() const;
	
public:
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FJVersioningSettings Versioning;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FJSettings AssetSettings;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FJPresetSettings Preset;

	UPROPERTY(EditAnywhere, Config, Category = BlueprintImport)
	FJBlueprintImportSettings BlueprintImport;

	UPROPERTY(EditAnywhere, Config, Category = CompatibilityFallback, AdvancedDisplay)
	FJCompatibilityFallbackSettings CompatibilityFallback;

	UPROPERTY(EditAnywhere, Config, Category = Redirectors, meta = (TitleProperty = "Name"))
	TArray<FJRedirector> Redirectors;

	/* Retrieves assets from an API and imports references directly into your project. */
	UPROPERTY(EditAnywhere, Config, Category = Cloud, DisplayName = "Enable Cloud")
	bool EnableCloudServer = true;

	/* Enables experimental/developing features of JsonAsAsset. Features may not work as intended. */
	UPROPERTY(EditAnywhere, Config, Category = Settings, AdvancedDisplay)
	bool EnableExperiments = false;
};
