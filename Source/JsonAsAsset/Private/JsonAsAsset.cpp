/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "JsonAsAsset.h"

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#if ENGINE_UE4
#if !UE4_18_BELOW
#include "ToolMenus.h"
#endif
#include "LevelEditor.h"
#endif

#include "Http.h"
#include "Modules/Versioning.h"

#include "Modules/UI/StyleModule.h"
#include "Modules/Toolbar/Toolbar.h"
#include "Utilities/EngineUtilities.h"

#include "Logging/LogVerbosity.h"
#include "Logging/LogMacros.h"
#include "Utilities/Compatibility.h"
/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

#ifdef _MSC_VER
#undef GetObject
#endif

#if UE4_18_BELOW
FString GitHub::URL = TEXT("https://github.com/JsonAsAsset/JsonAsAsset");
FString GitHub::README::Link = GitHub::URL + TEXT("?tab=readme-ov-file#asset-types");
FString GitHub::README::AssetTypes = GitHub::README::Link + TEXT("#asset-types");
FString GitHub::README::Cloud = GitHub::README::Link + TEXT("#cloud");
FString Donation::KO_FI = TEXT("https://ko-fi.com/t4ctor");
#endif

void FJsonAsAssetModule::StartupModule() {
	LogHttp.SetVerbosity(ELogVerbosity::Error);

	FJMetadata::Initialize();
	
    /* Initialize plugin style, reload textures */
    FJsonAsAssetStyle::Initialize();
    FJsonAsAssetStyle::ReloadTextures();

    /* Register Toolbar */
	Toolbar = NewObject<UJsonAsAssetToolbar>();
	Toolbar->AddToRoot();
	
#if ENGINE_UE5
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateUObject(Toolbar, &UJsonAsAssetToolbar::Register));
#else
	{
    	const TSharedPtr<FUICommandList> PluginCommands = MakeShareable(new FUICommandList);

    	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
    	const TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    	ToolbarExtender->AddToolBarExtension(
			"Settings",
			EExtensionHook::After,
			PluginCommands,
			FToolBarExtensionDelegate::CreateUObject(Toolbar, &UJsonAsAssetToolbar::UE4Register)
		);

    	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
	}
#endif

    const UJsonAsAssetSettings* Settings = GetSettings();
	
	if (!Settings->Versioning.Disable) {
		GJsonAsAssetVersioning.Update();
	}
}

void FJsonAsAssetModule::ShutdownModule() {
	/* Unregister startup callback and tool menus */
#if !UE4_18_BELOW
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
#endif

	/* Shutdown the plugin style */
	FJsonAsAssetStyle::Shutdown();

	if (Toolbar) {
		Toolbar->RemoveFromRoot();
		Toolbar = nullptr;
	}
}

IMPLEMENT_MODULE(FJsonAsAssetModule, JsonAsAsset)
