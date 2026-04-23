/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Constructor/ImportReader.h"

#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/TemplatedImporter.h"
#include "Importers/Types/Blueprint/AnimationBlueprintImporter.h"
#include "Importers/Types/Blueprint/BlueprintImporter.h"
#include "Importers/Types/Blueprint/WidgetBlueprintImporter.h"
#include "Importers/Types/DataAssetImporter.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Settings/Runtime.h"
#include "Styling/SlateIconFinder.h"
#include "Utilities/AssetUtilities.h"
#include "Utilities/JsonUtilities.h"

enum class EImportResultState : uint8 {
  Imported,
  Skipped,
  Failed
};

namespace {
const TCHAR *ToResultText(const EImportResultState State) {
  switch (State) {
  case EImportResultState::Imported:
    return TEXT("imported");
  case EImportResultState::Skipped:
    return TEXT("skipped");
  case EImportResultState::Failed:
    return TEXT("failed");
  default:
    return TEXT("unknown");
  }
}

FString NormalizeFilePath(const FString &FilePath) {
  FString Normalized = FilePath;
  if (Normalized.Contains(TEXT("\\"))) {
    Normalized = Normalized.Replace(TEXT("\\"), TEXT("/"));
  }
  if (FPaths::IsRelative(Normalized)) {
    Normalized = FPaths::ConvertRelativePathToFull(Normalized);
    Normalized = Normalized.Replace(TEXT("\\"), TEXT("/"));
  }
  return Normalized;
}

void LogImportResult(const EImportResultState State, const FString &Name,
                     const FString &Type, const FString &File,
                     const FString &Detail = TEXT("")) {
  const TCHAR *StateText = ToResultText(State);
  const FString Message = FString::Printf(
      TEXT("[ImportResult:%s] asset='%s' type='%s' file='%s'"), StateText,
      *Name, *Type, *File);
  const FString FullMessage =
      Detail.IsEmpty()
          ? Message
          : Message + FString::Printf(TEXT(" detail='%s'"), *Detail);

  if (State == EImportResultState::Failed) {
    UE_LOG(LogJsonAsAsset, Error, TEXT("%s"), *FullMessage);
  } else if (State == EImportResultState::Skipped) {
    UE_LOG(LogJsonAsAsset, Warning, TEXT("%s"), *FullMessage);
  } else {
    UE_LOG(LogJsonAsAsset, Log, TEXT("%s"), *FullMessage);
  }
}

bool TryExtractExportRootFromFile(const FString &File, const FString &Marker,
                                  FString &OutExportRoot) {
  FString Prefix;
  if (!File.Split(*Marker, &Prefix, nullptr, ESearchCase::IgnoreCase,
                  ESearchDir::FromEnd)) {
    return false;
  }

  OutExportRoot = Prefix + Marker.LeftChop(1);
  return !OutExportRoot.IsEmpty();
}

UPackage *ResolveAssetPackageWithFallback(const FString &Name,
                                          const FString &File,
                                          FString &FailureReason) {
  UPackage *LocalPackage =
      FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);
  if (LocalPackage) {
    return LocalPackage;
  }

  UJsonAsAssetSettings *PluginSettings = GetSettings();
  GJsonAsAssetRuntime.Update();
  if (PluginSettings) {
    PluginSettings->ApplyPresetRuntimeOverrides();
  }

  LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);
  if (LocalPackage) {
    return LocalPackage;
  }

  FString ExportDirectoryCache = GJsonAsAssetRuntime.ExportDirectory.Path;
  bool bTouchedRuntimeExportDir = false;

  TArray<FString> ExportMarkers = {TEXT("Output/Exports/")};
  if (GJsonAsAssetRuntime.IsBetterMartPresetActive()) {
    ExportMarkers.Add(TEXT("Exports/"));
  }

  for (const FString &Marker : ExportMarkers) {
    FString DirectoryPathFix;
    if (!TryExtractExportRootFromFile(File, Marker, DirectoryPathFix)) {
      continue;
    }

    bTouchedRuntimeExportDir = true;
    GJsonAsAssetRuntime.ExportDirectory.Path = DirectoryPathFix;
    if (PluginSettings) {
      SavePluginSettings(PluginSettings);
    }

    LocalPackage =
        FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);
    if (LocalPackage) {
      UE_LOG(LogJsonAsAsset, Log,
             TEXT("Resolved package root via marker '%s': '%s'"),
             *Marker, *DirectoryPathFix);
      return LocalPackage;
    }
  }

  if (bTouchedRuntimeExportDir) {
    GJsonAsAssetRuntime.ExportDirectory.Path = ExportDirectoryCache;
    if (PluginSettings) {
      SavePluginSettings(PluginSettings);
    }
  }

  return nullptr;
}
} // namespace

static UClass *ResolveClassFromExportMetadata(FUObjectExport *Export) {
  if (!Export || !Export->Has(TEXT("Class"))) {
    return nullptr;
  }

  FString ClassField = Export->GetString(TEXT("Class"));
  if (ClassField.IsEmpty()) {
    return nullptr;
  }

  FString ClassObjectPath = ClassField;
  if (ClassField.Contains(TEXT("'"))) {
    if (!ClassField.Split(TEXT("'"), nullptr, &ClassObjectPath,
                          ESearchCase::CaseSensitive, ESearchDir::FromStart)) {
      return nullptr;
    }
    ClassObjectPath.Split(TEXT("'"), &ClassObjectPath, nullptr,
                          ESearchCase::CaseSensitive, ESearchDir::FromStart);
  }

  if (ClassObjectPath.IsEmpty()) {
    return nullptr;
  }

  if (UClass *LoadedClass =
          StaticLoadClass(UObject::StaticClass(), nullptr, *ClassObjectPath)) {
    return LoadedClass;
  }

#if UE5_6_BEYOND
  return FindFirstObject<UClass>(*ClassObjectPath);
#else
  return FindObject<UClass>(ANY_PACKAGE, *ClassObjectPath);
#endif
}

bool IImportReader::ReadExportsAndImport(
    const TArray<TSharedPtr<FJsonValue>> &Exports, const FString &File,
    IImporter *&OutImporter, const bool HideNotifications) {
  if (UJsonAsAssetSettings *Settings = GetSettings()) {
    Settings->ApplyPresetRuntimeOverrides();
  }

  OutImporter = nullptr;

  if (Exports.Num() == 0) {
    LogImportResult(EImportResultState::Skipped, TEXT("<none>"),
                    TEXT("<none>"), File, TEXT("No exports in JSON payload."));
    return false;
  }

  FUObjectExportContainer *Container = new FUObjectExportContainer(Exports);
  if (Container == nullptr || Container->Exports.Num() == 0) {
    LogImportResult(EImportResultState::Skipped, TEXT("<none>"),
                    TEXT("<none>"), File,
                    TEXT("Export container construction yielded zero exports."));
    return false;
  }

  bool bImportedAny = false;

  bool HasBlueprintGeneratedClassExport = false;
  for (FUObjectExport *Export : Container->Exports) {
    if (Export && Export->GetType().ToString().Contains(
                      TEXT("BlueprintGeneratedClass"))) {
      HasBlueprintGeneratedClassExport = true;
      break;
    }
  }

  for (FUObjectExport *Export : Container->Exports) {
    if (!Export || !Export->IsJsonValid()) {
      continue;
    }

    if (HasBlueprintGeneratedClassExport) {
      if (!Export->GetType().ToString().Contains(
              TEXT("BlueprintGeneratedClass"))) {
        continue;
      }
    }

    if (IImporter *Importer =
            ReadExportAndImport(Container, Export, File, HideNotifications)) {
      OutImporter = Importer;
      bImportedAny = true;
    }
  }

  if (!bImportedAny) {
    LogImportResult(EImportResultState::Skipped, TEXT("<none>"),
                    TEXT("<none>"), File,
                    TEXT("No compatible exports were imported."));
  }

  return bImportedAny;
}

IImporter *
IImportReader::ReadExportAndImport(FUObjectExportContainer *Container,
                                   FUObjectExport *Export, FString File,
                                   const bool HideNotifications) {
  if (!Container || !Export || !Export->IsJsonValid()) {
    LogImportResult(EImportResultState::Skipped, TEXT("<invalid>"),
                    TEXT("<unknown>"), File,
                    TEXT("Invalid export or container."));
    return nullptr;
  }

  const FString Type = Export->GetType().ToString();
  FString Name = Export->GetName().ToString();

  const bool IsBlueprint = Type.Contains("BlueprintGeneratedClass");

  /* BlueprintGeneratedClass is post-fixed with _C */
  if (IsBlueprint) {
    Name.Split("_C", &Name, nullptr, ESearchCase::CaseSensitive,
               ESearchDir::FromEnd);
  }

  const UClass *ExportClass = Export->GetClass();
  const UClass *MetadataClass =
      ExportClass ? nullptr : ResolveClassFromExportMetadata(Export);
  const UClass *Class =
      ExportClass ? ExportClass
                  : (MetadataClass ? MetadataClass : FindClassByType(Type));

  if (Class == nullptr) {
    LogImportResult(EImportResultState::Skipped, Name, Type, File,
                    TEXT("Failed to resolve export class."));
    return nullptr;
  }

  /* Check if this export can be imported */
  const bool InheritsDataAsset = Class->IsChildOf(UDataAsset::StaticClass());
  if (!CanImport(Type, false, Class)) {
    LogImportResult(
        EImportResultState::Skipped, Name, Type, File,
        FString::Printf(TEXT("No importer registered for resolved class '%s'."),
                        *Class->GetName()));
    return nullptr;
  }

  File = NormalizeFilePath(File);

  FString FailureReason;
  UPackage *LocalPackage =
      ResolveAssetPackageWithFallback(Name, File, FailureReason);

  if (LocalPackage == nullptr) {
    LogImportResult(EImportResultState::Failed, Name, Type, File,
                    FailureReason.IsEmpty()
                        ? TEXT("Failed to create package from file path.")
                        : FailureReason);
    AppendNotification(
        FText::FromString("Failed: " + Type), FText::FromString(FailureReason),
        4.0f,
        FSlateIconFinder::FindCustomIconBrushForClass(
            FindObject<UClass>(nullptr, *("/Script/Engine." + Type)),
            TEXT("ClassThumbnail")),
        SNotificationItem::CS_Fail, false, 350.0f);

    return nullptr;
  }

  /* Importer
   * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   */
  IImporter *Importer = nullptr;

  /* Try to find the importer using a factory delegate */
  if (const FImporterFactoryDelegate *Factory = FindFactoryForAssetType(Type)) {
    Importer = (*Factory)();
  }

  /* Route blueprint-family generated classes through the proper importer even
   * when the exact type isn't registered. */
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

  /* By default, (with no existing importer) use the templated importer with the
   * asset class. */
  if (Importer == nullptr) {
    Importer = new ITemplatedImporter<UObject>();
  }

  /* TODO: Don't hardcode this. */
  if (ImportTypes::Cloud::Extra.Contains(Type)) {
    Importer = new ITextureImporter<UTextureLightProfile>();
  }

  Export->Package = LocalPackage;
  Importer->Initialize(Export, Container);

  /* Import the asset
   * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
  bool Successful = false;
  {
    try {
      Successful = Importer->Import();
    } catch (const char *Exception) {
      UE_LOG(LogJsonAsAsset, Error,
             TEXT("Importer exception while importing '%s' as '%s': %s"), *Name,
             *Type, *FString(Exception));
      Successful = false;
    } catch (...) {
      UE_LOG(LogJsonAsAsset, Error,
             TEXT("Importer exception while importing '%s' as '%s': unknown exception"),
             *Name, *Type);
      Successful = false;
    }
  }

  if (HideNotifications) {
    LogImportResult(Successful ? EImportResultState::Imported
                               : EImportResultState::Failed,
                    Name, Type, File,
                    Successful ? TEXT("Hidden notifications mode.")
                               : TEXT("Importer returned failure."));
    return Successful ? Importer : nullptr;
  }

  if (Successful) {
    LogImportResult(EImportResultState::Imported, Name, Type, File);

    /* Successful Notification */
    AppendNotification(
        FText::FromString("Imported: " + Name), FText::FromString(Type), 2.0f,
        FSlateIconFinder::FindCustomIconBrushForClass(
            FindObject<UClass>(nullptr, *("/Script/Engine." + Type)),
            TEXT("ClassThumbnail")),
        SNotificationItem::CS_Success, false, 350.0f);
  } else {
    LogImportResult(
        EImportResultState::Failed, Name, Type, File,
        TEXT("Importer returned failure. See previous logs for details."));

    /* Failed Notification */
    AppendNotification(
        FText::FromString("Failed: " + Name), FText::FromString(Type), 2.0f,
        FSlateIconFinder::FindCustomIconBrushForClass(
            FindObject<UClass>(nullptr, *("/Script/Engine." + Type)),
            TEXT("ClassThumbnail")),
        SNotificationItem::CS_Fail, false, 350.0f);
  }

  return Successful ? Importer : nullptr;
}

IImporter *IImportReader::ImportReference(const FString &File) {
  if (UJsonAsAssetSettings *Settings = GetSettings()) {
    Settings->ApplyPresetRuntimeOverrides();
  }

  FString FilePath = NormalizeFilePath(File);

  if (!FPaths::FileExists(FilePath)) {
    LogImportResult(EImportResultState::Failed, TEXT("<none>"),
                    TEXT("<unknown>"), FilePath,
                    TEXT("File does not exist."));
    return nullptr;
  }

  if (!FilePath.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase)) {
    LogImportResult(EImportResultState::Skipped, TEXT("<none>"),
                    TEXT("<unknown>"), FilePath,
                    TEXT("Only .json exports are supported by local import."));
    return nullptr;
  }

  TArray<TSharedPtr<FJsonValue>> DataObjects;
  {
    DeserializeJSON(FilePath, DataObjects);
  }

  if (DataObjects.Num() == 0) {
    LogImportResult(EImportResultState::Failed, TEXT("<none>"),
                    TEXT("<unknown>"), FilePath,
                    TEXT("JSON parsed but no export entries were deserialized."));
    return nullptr;
  }

  IImporter *Importer = nullptr;
  const bool bImported = ReadExportsAndImport(DataObjects, FilePath, Importer);
  if (!bImported) {
    LogImportResult(EImportResultState::Skipped, TEXT("<none>"),
                    TEXT("<unknown>"), FilePath,
                    TEXT("No exports were imported from file."));
  }

  return Importer;
}
