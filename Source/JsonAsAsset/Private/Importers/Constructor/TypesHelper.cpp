/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Constructor/TypesHelper.h"

#include "Importers/Constructor/Types.h"
#include "Importers/Constructor/Registry/RegistrationInfo.h"
#include "Engine/Compatibility.h"

bool CanImport(const FString& Type, const bool IsCloud, const UClass* Class, FString* OutReason) {
	auto SetReason = [OutReason](const FString& Reason) {
		if (OutReason != nullptr) {
			*OutReason = Reason;
		}
	};

	if (IsCloud) {
		if (!ImportTypes::Cloud::Allowed(Type)) {
			SetReason(FString::Printf(TEXT("Type '%s' is blacklisted for cloud imports."), *Type));
			return false;
		}

		if (Type == "SoundWave") {
			SetReason(TEXT("Cloud import explicitly allows SoundWave."));
			return true;
		}
	}
    
	if (FindFactoryForAssetType(Type)) {
		SetReason(TEXT("Matched a registered importer factory."));
		return true;
	}
    
	for (const TPair<FString, TArray<FString>>& Pair : ImportTypes::Templated) {
		if (Pair.Value.Contains(Type)) {
			SetReason(FString::Printf(TEXT("Matched templated import category '%s'."), *Pair.Key));
			return true;
		}
	}

	if (!Class) {
		Class = FindClassByType(Type);
	}

	if (Class == nullptr) {
		SetReason(FString::Printf(TEXT("Could not resolve Unreal class for type '%s'."), *Type));
		return false;
	}

	if (ImportTypes::Cloud::Extra.Contains(Type)) {
		SetReason(TEXT("Matched cloud-extra import type."));
		return true;
	}

	if (!ImportTypes::Allowed(Type)) {
		SetReason(FString::Printf(TEXT("Type '%s' is gated behind experimental imports."), *Type));
		return false;
	}
	
	if (Class->IsChildOf(UDataAsset::StaticClass())) {
		SetReason(TEXT("Class derives from UDataAsset."));
		return true;
	}
	
	if (Class->IsChildOf(UTexture::StaticClass())) {
		SetReason(TEXT("Direct JSON import for UTexture-derived classes is disabled; use cloud-backed texture import flow."));
		return false;
	}

	/* Lots of issues happened */
#if 0
	/* If the Class has an asset type action, it's importable */
	const FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	const IAssetTools& AssetTools = AssetToolsModule.Get();
	
	static TArray<TWeakPtr<IAssetTypeActions>> OutAssetTypeActionsList = {};
	AssetTools.GetAssetTypeActionsList(OutAssetTypeActionsList);

	for (TWeakPtr AssetTypeAction : OutAssetTypeActionsList) {
		if (const TSharedPtr<IAssetTypeActions> Action = AssetTypeAction.Pin()) {
			const UClass* SupportedClass = Action->GetSupportedClass();

			if (SupportedClass == Class) return true;
		}
	}
#endif

	SetReason(FString::Printf(TEXT("No importer is registered for type '%s' (class '%s')."), *Type, *Class->GetName()));
	return false;
}
