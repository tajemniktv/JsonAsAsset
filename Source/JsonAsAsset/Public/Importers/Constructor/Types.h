/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "Utilities/Compatibility.h"

struct ImportTypes {
	/* AssetType/Category ~ Defined in CPP */
#if !UE4_18_BELOW
	static TMap<FString, TArray<FString>> Templated;
#else
	static TMap<FString, TArray<FString>>& GetImporterTemplatedTypes();
#endif
	
	struct Cloud {
#if UE4_18_BELOW
		static TArray<FString> Blacklisted;
		static TArray<FString> Extra;
#else
		static inline TArray<FString> Blacklisted = {
			"AnimSequence",
			"AnimMontage",
			"AnimBlueprintGeneratedClass"
		};

		static inline TArray<FString> Extra = {
			"TextureLightProfile"
		};
#endif

		static bool Allowed(const FString& Type);
	};

#if UE4_18_BELOW
	static TArray<FString> Experimental;
#else
	static inline TArray<FString> Experimental = {
		"AnimBlueprintGeneratedClass"
	};
#endif

	static bool Allowed(const FString& ImporterType);
};