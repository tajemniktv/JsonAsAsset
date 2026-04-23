/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "ApplicationUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Modules/Log.h"
#include "RemoteUtilities.h"

inline void BrowseToAsset(UObject *Asset) {
  if (Asset == nullptr || !IsValid(Asset)) {
    UE_LOG(LogJsonAsAsset, Warning,
           TEXT("BrowseToAsset skipped: invalid asset pointer."));
    return;
  }

  /* Browse to newly added Asset in the Content Browser */
  const FAssetData AssetData(Asset);
  if (!AssetData.IsValid()) {
    UE_LOG(LogJsonAsAsset, Warning,
           TEXT("BrowseToAsset skipped: invalid asset data for '%s'."),
           *Asset->GetName());
    return;
  }

  const TArray<FAssetData> Assets = {AssetData};
  const FContentBrowserModule &ContentBrowserModule =
      FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>(
          "ContentBrowser");
  ContentBrowserModule.Get().SyncBrowserToAssets(Assets);
}

/* Get the asset currently selected in the Content Browser. */
template <typename T>
T *GetSelectedAsset(const bool SuppressErrors = false,
                    FString OptionalAssetNameCheck = "") {
  const FContentBrowserModule &ContentBrowserModule =
      FModuleManager::LoadModuleChecked<FContentBrowserModule>(
          "ContentBrowser");
  TArray<FAssetData> SelectedAssets;
  ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

  if (SelectedAssets.Num() == 0) {
    if (SuppressErrors == true) {
      return nullptr;
    }

    GLog->Log(
        "JsonAsAsset: [GetSelectedAsset] None selected, returning nullptr.");

    const FText DialogText = FText::Format(
        FText::FromString(
            TEXT("Importing an asset of type '{0}' requires a base asset "
                 "selected to modify. Select one in your content browser.")),
        FText::FromString(T::StaticClass()->GetName()));

    FMessageDialog::Open(EAppMsgType::Ok, DialogText);

    return nullptr;
  }

  UObject *SelectedAsset = SelectedAssets[0].GetAsset();
  T *CastedAsset = Cast<T>(SelectedAsset);

  if (!CastedAsset) {
    if (SuppressErrors == true) {
      return nullptr;
    }

    GLog->Log("JsonAsAsset: [GetSelectedAsset] Selected asset is not of the "
              "required class, returning nullptr.");

    const FText DialogText = FText::Format(
        FText::FromString(TEXT("The selected asset is not of type '{0}'. "
                               "Please select a valid asset.")),
        FText::FromString(T::StaticClass()->GetName()));

    FMessageDialog::Open(EAppMsgType::Ok, DialogText);

    return nullptr;
  }

  if (CastedAsset && OptionalAssetNameCheck != "" &&
      !CastedAsset->GetName().Equals(OptionalAssetNameCheck)) {
    CastedAsset = nullptr;
  }

  return CastedAsset;
}

/* Gets all assets in selected folder */
inline TArray<FAssetData> GetAssetsInSelectedFolder() {
  TArray<FAssetData> AssetDataList;

  /* Get the Content Browser Module */
  const FContentBrowserModule &ContentBrowserModule =
      FModuleManager::LoadModuleChecked<FContentBrowserModule>(
          "ContentBrowser");

  TArray<FString> SelectedFolders;
  ContentBrowserModule.Get().GetSelectedPathViewFolders(SelectedFolders);

  if (SelectedFolders.Num() == 0) {
    UE_LOG(LogJsonAsAsset, Warning,
           TEXT("No folder selected in the Content Browser."));
    return AssetDataList;
  }

  FString CurrentFolder = SelectedFolders[0];

#if ENGINE_UE5
  /* Convert virtual paths to internal package paths */
  const UContentBrowserDataSubsystem *ContentBrowserData =
      GEditor->GetEditorSubsystem<UContentBrowserDataSubsystem>();

  if (!ContentBrowserData) {
    return AssetDataList;
  }

  TArray<FString> InternalPaths =
      ContentBrowserData->TryConvertVirtualPathsToInternal(SelectedFolders);
  if (InternalPaths.Num() == 0) {
    UE_LOG(LogJsonAsAsset, Warning,
           TEXT("Unable to resolve selected Content Browser folder to an "
                "internal path."));
    return AssetDataList;
  }

  CurrentFolder = InternalPaths[0];
#endif

  /* Check if the folder is the root folder, and show a prompt if */
  if (CurrentFolder == "/All/Game") {
    bool Continue = false;

    SpawnYesNoPrompt(
        TEXT("Large Operation"),
        TEXT("This will stall the editor for a long time. Continue anyway?"),
        [&](const bool Confirmed) { Continue = Confirmed; });

    if (!Continue) {
      UE_LOG(LogJsonAsAsset, Warning, TEXT("Action cancelled by user."));
      return AssetDataList;
    }
  }

  /* Get the Asset Registry Module */
  const FAssetRegistryModule &AssetRegistryModule =
      FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
  AssetRegistryModule.Get().SearchAllAssets(true);

  /* Get all assets in the folder and its subfolders */
  AssetRegistryModule.Get().GetAssetsByPath(FName(*CurrentFolder),
                                            AssetDataList, true);

  return AssetDataList;
}