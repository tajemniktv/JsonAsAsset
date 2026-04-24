/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"

#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Types/Materials/MaterialApproximation.h"

#if ENGINE_UE4
#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"
#endif

#include "Engine/EngineUtilities.h"

#include "Modules/Toolbar/Tools/ClearImportData.h"
#include "Modules/Toolbar/Tools/FixUpAssetData.h"
#include "Utilities/DialogUtilities.h"

void IToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	UJsonAsAssetSettings* Settings = GetSettings();
	
	MenuBuilder.AddSubMenu(
		FText::FromString("Asset Tools"),
		FText::FromString("Tools bundled"),
		FNewMenuDelegate::CreateLambda([this, Settings](FMenuBuilder& InnerMenuBuilder) {
			InnerMenuBuilder.BeginSection("JsonAsAssetToolsSection", FText::FromString("Tools"));
			{
				InnerMenuBuilder.AddMenuEntry(
					FText::FromString("Clear Import Data"),
					FText::FromString(""),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

					FUIAction(
						FExecuteAction::CreateLambda([] {
							TToolClearImportData* Tool = new TToolClearImportData();
							Tool->Execute();
						})
					),
					NAME_None
				);

				InnerMenuBuilder.AddMenuEntry(
					FText::FromString("Fixup Asset Data"),
					FText::FromString(""),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

					FUIAction(
						FExecuteAction::CreateLambda([] {
							TToolFixUpAssetData* Tool = new TToolFixUpAssetData();
							Tool->Execute();
						})
					),
					NAME_None
				);

				InnerMenuBuilder.AddMenuEntry(
					FText::FromString("Import Folder of JSON Files"),
					FText::FromString(""),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

					FUIAction(
						FExecuteAction::CreateLambda([] {
							for (FString Folder : OpenFolderDialog("Folder of JSON files")) {
								TArray<FString> JsonFiles;
								IFileManager::Get().FindFilesRecursive(
									JsonFiles,
									*Folder,
									TEXT("*.json"),
									true,
									true,
									false
								);

								FScopedMaterialApproximationImportSession ApproximationSession(JsonFiles);
								JsonFiles.Sort([](const FString& A, const FString& B) {
									const int32 RankA = FMaterialApproximation::GetImportRankForFile(A);
									const int32 RankB = FMaterialApproximation::GetImportRankForFile(B);
									if (RankA != RankB) {
										return RankA < RankB;
									}
									return A < B;
								});

								for (FString& JsonPath : JsonFiles) {
									IImportReader::ImportReference(JsonPath);
								}
							}
						})
					),
					NAME_None
				);

				InnerMenuBuilder.EndSection();
			}
		}),
		false,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "DeveloperTools.MenuIcon")
	);
}
