/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Constructor/Types.h"

#include "Settings/JsonAsAssetSettings.h"
#include "Utilities/EngineUtilities.h"

#if UE4_18_BELOW
TArray<FString> ImportTypes::Cloud::Blacklisted = {
			"AnimSequence",
			"AnimMontage",
			"AnimBlueprintGeneratedClass"
};
TArray<FString> ImportTypes::Cloud::Extra = {
			"TextureLightProfile"
};
TArray<FString> ImportTypes::Experimental = {
		"AnimBlueprintGeneratedClass"
};
#endif

/* Define supported template asset class here */
#if UE4_18_BELOW
TMap<FString, TArray<FString>>& ImportTypes::GetImporterTemplatedTypes()
{
	static TMap<FString, TArray<FString>> ImporterTemplatedTypes;

	if (ImporterTemplatedTypes.Num() == 0)
	{
		ImporterTemplatedTypes.Add(TEXT("Curve Assets"), {
			TEXT("CurveFloat")
			});

		ImporterTemplatedTypes.Add(TEXT("Data Assets"), {
			TEXT("SlateBrushAsset"),
			TEXT("SlateWidgetStyleAsset")
			});

		ImporterTemplatedTypes.Add(TEXT("Landscape Assets"), {
			TEXT("LandscapeGrassType"),
			TEXT("FoliageType_InstancedStaticMesh"),
			TEXT("FoliageType_Actor")
			});

		ImporterTemplatedTypes.Add(TEXT("Material Assets"), {
			TEXT("MaterialParameterCollection"),
			TEXT("SubsurfaceProfile")
			});

		ImporterTemplatedTypes.Add(TEXT("Skeletal Assets"), {
			TEXT("SkeletalMeshLODSettings")
			});

		ImporterTemplatedTypes.Add(TEXT("Physics Assets"), {
			TEXT("PhysicalMaterial")
			});
		ImporterTemplatedTypes.Add(TEXT("Sound Assets"), {
			TEXT("ReverbEffect"),
			TEXT("SoundAttenuation"),
			TEXT("SoundConcurrency"),
			TEXT("SoundClass"),
			TEXT("SoundMix"),
			TEXT("SoundModulationPatch"),
			TEXT("SubmixEffectDynamicsProcessorPreset")
			});

		ImporterTemplatedTypes.Add(TEXT("Texture Assets"), {
			TEXT("TextureRenderTarget2D"),
			TEXT("RuntimeVirtualTexture")
			});

		ImporterTemplatedTypes.Add(TEXT("Sequencer Assets"), {
			TEXT("CameraAnim")
			});
	}

	return ImporterTemplatedTypes;
}
#else
TMap<FString, TArray<FString>> ImportTypes::Templated = {
	{
		TEXT("Curve Assets"),
		{
			TEXT("CurveFloat"),
			TEXT("CurveLinearColor"),
		}
	},
	{
		TEXT("Data Assets"),
		{
			TEXT("SlateBrushAsset"),
			TEXT("SlateWidgetStyleAsset"),
			TEXT("LandscapeLayerInfoObject"),
			TEXT("HLODProxy"),
			TEXT("AnimBoneCompressionSettings"),
			TEXT("AnimCurveCompressionSettings"),
		}
	},
	{
		TEXT("Landscape Assets"),
		{
			TEXT("LandscapeGrassType"),
			TEXT("FoliageType_InstancedStaticMesh"),
			TEXT("FoliageType_Actor"),
		}
	},
	{
		TEXT("Material Assets"),
		{
			TEXT("MaterialParameterCollection"),
			TEXT("SubsurfaceProfile"),
		}
	},
	{
		TEXT("Skeletal Assets"),
		{
			TEXT("SkeletalMeshLODSettings"),
		}
	},
	{
		TEXT("Physics Assets"),
		{
			TEXT("PhysicalMaterial"),
		}
	},
{
		TEXT("UI Assets"),
		{
			TEXT("Font"),
		}
	},
	{
		TEXT("Sound Assets"),
		{
			TEXT("ReverbEffect"),
			TEXT("SoundAttenuation"),
			TEXT("SoundConcurrency"),
			TEXT("SoundClass"),
			TEXT("SoundMix"),
			TEXT("SoundModulationPatch"),
			TEXT("SubmixEffectDynamicsProcessorPreset"),
		}
	},
	{
		TEXT("Texture Assets"),
		{
			TEXT("TextureRenderTarget2D"),
			TEXT("RuntimeVirtualTexture"),
		}
	},
	{
		TEXT("Sequencer Assets"),
		{
			TEXT("CameraAnim"),
			TEXT("ForceFeedbackEffect")
		}
	}
}
#endif

bool ImportTypes::Cloud::Allowed(const FString& Type) {
	if (Blacklisted.Contains(Type)) {
		return false;
	}

	if (Extra.Contains(Type)) {
		return true;
	}

	return true;
}

bool ImportTypes::Allowed(const FString& ImporterType) {
	if (Experimental.Contains(ImporterType)) {
		const UJsonAsAssetSettings* Settings = GetSettings();

		if (!Settings->EnableExperiments) return false;
	}

	return true;
}
