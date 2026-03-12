/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Toolbar.h"
#include "Utilities/Compatibility.h"

#if ENGINE_UE4
#include "Modules/ModuleInterface.h"
#endif

struct GitHub {
#if UE4_18_BELOW
	static FString URL;
#else
    static inline FString URL = "https://github.com/JsonAsAsset/JsonAsAsset";
#endif
    
    struct README {
#if UE4_18_BELOW
		static FString Link;
		static FString AssetTypes;
		static FString Cloud;
#else
        static inline FString Link = URL + "?tab=readme-ov-file#asset-types";
        static inline FString AssetTypes = Link + "#asset-types";
        static inline FString Cloud = Link + "#cloud";
#endif
    };
};

struct Donation {
#if UE4_18_BELOW
	static FString KO_FI;
#else
    static inline FString KO_FI = "https://ko-fi.com/t4ctor";
#endif
};

class FJsonAsAssetModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    UJsonAsAssetToolbar* Toolbar = nullptr;
};