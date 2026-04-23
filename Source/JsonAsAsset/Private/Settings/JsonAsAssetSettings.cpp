/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Settings/JsonAsAssetSettings.h"
#include "Modules/Log.h"
#include "Modules/Metadata.h"
#include "Settings/Runtime.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"

FName GJsonAsAssetSettingsCategoryName = FName("General");
FName GJsonAsAssetInternalName = FName("AmbientAudio");

UJsonAsAssetSettings::UJsonAsAssetSettings() {
	CategoryName = GJsonAsAssetSettingsCategoryName;
	SectionName = GJsonAsAssetName;
}

FText UJsonAsAssetSettings::GetSectionText() const {
	return FText::FromString(GJsonAsAssetName.ToString());
}

bool UJsonAsAssetSettings::IsBetterMartPresetEnabled() const {
	return Preset.Profile == EJImportPreset::BetterMartMirror;
}

void UJsonAsAssetSettings::ApplyPresetRuntimeOverrides() const {
	const bool bEnableBetterMart = IsBetterMartPresetEnabled();

	GJsonAsAssetRuntime.bBetterMartPresetActive = bEnableBetterMart;
	GJsonAsAssetRuntime.Profile.Name = bEnableBetterMart ? TEXT("BetterMart") : TEXT("");

	static bool bLoggedLastState = false;
	static bool bWasBetterMart = false;
	if (!bLoggedLastState || bWasBetterMart != bEnableBetterMart) {
		if (bEnableBetterMart) {
			UE_LOG(LogJsonAsAsset, Log,
			       TEXT("[Preset] BetterMartMirror active. Runtime profile='%s'; Blueprint-family compatibility fallbacks are treated as enabled by preset defaults."),
			       *GJsonAsAssetRuntime.Profile.Name);
		} else {
			UE_LOG(LogJsonAsAsset, Log,
			       TEXT("[Preset] Default profile active. BetterMart runtime overrides are disabled."));
		}

		bWasBetterMart = bEnableBetterMart;
		bLoggedLastState = true;
	}
}
