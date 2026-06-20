/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Materials/Material.h"

class IMaterialImporter;

enum class EApproxTextureRole : uint8 {
	Unknown,
	BaseColor,
	Normal,
	ORM,
	Roughness,
	Metallic,
	AmbientOcclusion,
	Emissive,
	EmissiveBlinkers,
	Opacity,
	OpacityMask,
	Specular,
	ClearCoat,
	ClearCoatRoughness
};

enum class EApproxScalarRole : uint8 {
	Unknown,
	Roughness,
	Metallic,
	AmbientOcclusion,
	EmissiveIntensity,
	Opacity,
	ClearCoat,
	ClearCoatRoughness
};

enum class EApproxVectorRole : uint8 {
	Unknown,
	TintColor,
	EmissiveColor
};

struct FApproxTextureParam {
	FName ParameterName;
	FString TextureObjectPath;
	FString TextureName;
	EApproxTextureRole Role = EApproxTextureRole::Unknown;
	int32 UVChannel = 0;
	float SamplingScale = 1.0f;
};

struct FApproxScalarParam {
	FName ParameterName;
	float DefaultValue = 0.0f;
	EApproxScalarRole Role = EApproxScalarRole::Unknown;
};

struct FApproxVectorParam {
	FName ParameterName;
	FLinearColor DefaultValue = FLinearColor::White;
	EApproxVectorRole Role = EApproxVectorRole::Unknown;
};

struct FApproxStaticSwitchParam {
	FName ParameterName;
	bool DefaultValue = false;
};

struct FApproxMaterialModel {
	FString MaterialName;
	FString PackagePath;
	bool bTwoSided = false;
	EBlendMode BlendMode = BLEND_Opaque;
	EMaterialShadingModel ShadingModel = MSM_DefaultLit;
	int32 ShadingModelField = 0;
	float OpacityMaskClipValue = 0.3333f;
	TArray<FApproxTextureParam> Textures;
	TArray<FApproxScalarParam> Scalars;
	TArray<FApproxVectorParam> Vectors;
	TArray<FApproxStaticSwitchParam> StaticSwitches;
};

struct FApproxMaterialInstanceSummary : FApproxMaterialModel {
	TArray<FString> ParentMaterialObjectPaths;
};

class JSONASASSET_API FMaterialApproximationContext {
public:
	TMap<FString, FApproxMaterialModel> MaterialsByObjectPath;
	TMap<FString, TArray<FApproxMaterialInstanceSummary>> ChildrenByParentObjectPath;
	TMap<FString, int32> ImportRankByFile;
};

class JSONASASSET_API FScopedMaterialApproximationImportSession {
public:
	explicit FScopedMaterialApproximationImportSession(const TArray<FString>& JsonFiles);
	~FScopedMaterialApproximationImportSession();
};

class JSONASASSET_API FMaterialApproximation {
public:
	static EApproxTextureRole ClassifyTextureRole(const FString& Name);
	static EApproxScalarRole ClassifyScalarRole(const FString& Name);
	static EApproxVectorRole ClassifyVectorRole(const FString& Name);

	static FString NormalizeObjectPath(const FString& ObjectPath);
	static int32 GetImportRankForFile(const FString& JsonFile);

	static void BeginImportSession(const TArray<FString>& JsonFiles);
	static void EndImportSession();
	static const FMaterialApproximationContext* GetActiveContext();

	static bool TryApproximateMaterial(IMaterialImporter* MaterialImporter, const TSharedPtr<FJsonObject>& MaterialProperties);

private:
	static TUniquePtr<FMaterialApproximationContext> BuildContext(const TArray<FString>& JsonFiles);
};
