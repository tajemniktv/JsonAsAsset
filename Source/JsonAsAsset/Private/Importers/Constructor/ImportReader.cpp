/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Constructor/ImportReader.h"

#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/TemplatedImporter.h"
#include "Importers/Types/DataAssetImporter.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/Blueprint/BlueprintImporter.h"
#include "Importers/Types/Blueprint/AnimationBlueprintImporter.h"
#include "Importers/Types/Blueprint/WidgetBlueprintImporter.h"
#include "Settings/Runtime.h"
#include "Styling/SlateIconFinder.h"
#include "Utilities/AssetUtilities.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"

static UClass* ResolveClassFromExportMetadata(FUObjectExport* Export) {
	if (!Export || !Export->Has(TEXT("Class"))) {
		return nullptr;
	}

	FString ClassField = Export->GetString(TEXT("Class"));
	if (ClassField.IsEmpty()) {
		return nullptr;
	}

	FString ClassObjectPath = ClassField;
	if (ClassField.Contains(TEXT("'"))) {
		if (!ClassField.Split(TEXT("'"), nullptr, &ClassObjectPath, ESearchCase::CaseSensitive, ESearchDir::FromStart)) {
			return nullptr;
		}
		ClassObjectPath.Split(TEXT("'"), &ClassObjectPath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart);
	}

	if (ClassObjectPath.IsEmpty()) {
		return nullptr;
	}

	if (UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassObjectPath)) {
		return LoadedClass;
	}

#if UE5_6_BEYOND
	return FindFirstObject<UClass>(*ClassObjectPath);
#else
	return FindObject<UClass>(ANY_PACKAGE, *ClassObjectPath);
#endif
}

bool IImportReader::ReadExportsAndImport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& File, IImporter*& OutImporter, const bool HideNotifications) {
	FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);

	bool HasBlueprintGeneratedClassExport = false;
	for (FUObjectExport* Export : Container->Exports) {
		if (Export && Export->GetType().ToString().Contains(TEXT("BlueprintGeneratedClass"))) {
			HasBlueprintGeneratedClassExport = true;
			break;
		}
	}
	
	for (FUObjectExport* Export : Container->Exports) {
		if (HasBlueprintGeneratedClassExport) {
			if (!Export->GetType().ToString().Contains(TEXT("BlueprintGeneratedClass"))) {
				continue;
			}
		}
		
		if (IImporter* Importer = ReadExportAndImport(Container, Export, File, HideNotifications)) OutImporter = Importer;
	}

	return true;
}

IImporter* IImportReader::ReadExportAndImport(FUObjectExportContainer* Container, FUObjectExport* Export, FString File, const bool HideNotifications) {
	const FString Type = Export->GetType().ToString();
	FString Name = Export->GetName().ToString();

	const bool IsBlueprint = Type.Contains("BlueprintGeneratedClass");

	/* BlueprintGeneratedClass is post-fixed with _C */
	if (IsBlueprint) {
		Name.Split("_C", &Name, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	const UClass* ExportClass = Export->GetClass();
	const UClass* MetadataClass = ExportClass ? nullptr : ResolveClassFromExportMetadata(Export);
	const UClass* Class = ExportClass ? ExportClass : (MetadataClass ? MetadataClass : FindClassByType(Type));
	
	if (Class == nullptr) return nullptr;

	/* Check if this export can be imported */
	const bool InheritsDataAsset = Class->IsChildOf(UDataAsset::StaticClass());
	if (!CanImport(Type, false, Class)) return nullptr;

	/* Convert from relative path to full path */
	if (FPaths::IsRelative(File)) File = FPaths::ConvertRelativePathToFull(File);

	FString FailureReason;
	UPackage* LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

	if (LocalPackage == nullptr) {
		/* Try fixing our Export Directory Settings using the provided File directory if local package not found */
        UJsonAsAssetSettings* PluginSettings = GetSettings();

		GJsonAsAssetRuntime.Update();
		LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

		if (LocalPackage == nullptr) {
			FString ExportDirectoryCache = GJsonAsAssetRuntime.ExportDirectory.Path;
		
			if (FString DirectoryPathFix; File.Split(TEXT("Output/Exports/"), &DirectoryPathFix, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
				DirectoryPathFix = DirectoryPathFix + TEXT("Output/Exports");

				GJsonAsAssetRuntime.ExportDirectory.Path = DirectoryPathFix;
				SavePluginSettings(PluginSettings);

				/* Retry creating the asset package */
				LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

				/* Undo the change if unsuccessful */
				if (LocalPackage == nullptr) {
					GJsonAsAssetRuntime.ExportDirectory.Path = ExportDirectoryCache;

					SavePluginSettings(PluginSettings);
				}
			}
		}
	}

	if (LocalPackage == nullptr) {
		AppendNotification(
			FText::FromString("Failed: " + Type),
			FText::FromString(FailureReason),
			4.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Fail,
			false,
			350.0f
		);

		return nullptr;
	}

	/* Importer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	IImporter* Importer = nullptr;
	
	/* Try to find the importer using a factory delegate */
	if (const FImporterFactoryDelegate* Factory = FindFactoryForAssetType(Type)) {
		Importer = (*Factory)();
	}

	/* Route blueprint-family generated classes through the proper importer even when the exact type isn't registered. */
	if (Importer == nullptr && Type.Contains(TEXT("BlueprintGeneratedClass"))) {
		if (Type.Contains(TEXT("AnimBlueprintGeneratedClass"))) {
			Importer = new IAnimationBlueprintImporter();
		} else if (Type.Contains(TEXT("WidgetBlueprintGeneratedClass"))) {
			Importer = new IWidgetBlueprintImporter();
		} else {
			Importer = new IBlueprintImporter();
		}
	}

	/* If it inherits DataAsset, use the data asset importer */
	if (Importer == nullptr && InheritsDataAsset) {
		Importer = new IDataAssetImporter();
	}

	/* By default, (with no existing importer) use the templated importer with the asset class. */
	if (Importer == nullptr) {
		Importer = new ITemplatedImporter<UObject>();
	}

	/* TODO: Don't hardcode this. */
	if (ImportTypes::Cloud::Extra.Contains(Type)) {
		Importer = new ITextureImporter<UTextureLightProfile>();
	}

	Export->Package = LocalPackage;
	Importer->Initialize(Export, Container);

	/* Import the asset ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	bool Successful = false; {
		try {
			Successful = Importer->Import();
		} catch (const char* Exception) {
			UE_LOG(LogJsonAsAsset, Error, TEXT("Importer exception: %s"), *FString(Exception));
		}
	}

	if (HideNotifications) {
		return Importer;
	}

	if (Successful) {
		UE_LOG(LogJsonAsAsset, Log, TEXT("Successfully imported \"%s\" as \"%s\""), *Name, *Type);

		/* Successful Notification */
		AppendNotification(
			FText::FromString("Imported: " + Name),
			FText::FromString(Type),
			2.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Success,
			false,
			350.0f
		);
	} else {
		/* Failed Notification */
		AppendNotification(
			FText::FromString("Failed: " + Name),
			FText::FromString(Type),
			2.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Fail,
			false,
			350.0f
		);
	}

	return Importer;
}

IImporter* IImportReader::ImportReference(const FString& File) {
	FString FilePath = File;
	if (FilePath.Contains("\\")) {
		FilePath = File.Replace(TEXT("\\"), TEXT("/"));
	}
	
	TArray<TSharedPtr<FJsonValue>> DataObjects; {
		DeserializeJSON(FilePath, DataObjects);
	}

	IImporter* Importer = nullptr;
	ReadExportsAndImport(DataObjects, FilePath, Importer);
	
	return Importer;
}
