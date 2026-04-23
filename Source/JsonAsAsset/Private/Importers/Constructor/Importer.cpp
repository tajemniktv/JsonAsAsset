/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Constructor/Importer.h"

#include "Settings/JsonAsAssetSettings.h"

#include "Misc/MessageDialog.h"

/* ~~~~~~~~~~~~~ Templated Engine Classes ~~~~~~~~~~~~~ */
#include "Curves/CurveLinearColor.h"
#include "Engine/EngineUtilities.h"
#include "Engine/FontFace.h"
#include "Engine/SubsurfaceProfile.h"
#include "Materials/MaterialParameterCollection.h"
#include "Modules/Log.h"
#include "Sound/SoundNode.h"
#include <type_traits>

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

UObject *IImporter::CreateAsset(UObject *CreatedAsset) {
  if (CreatedAsset) {
    AssetExport->Object = CreatedAsset;

    return CreatedAsset;
  }

  return nullptr;
}

template void
IImporter::LoadExport<UMaterialInterface>(const TSharedPtr<FJsonObject> *,
                                          TObjectPtr<UMaterialInterface> &);
template void
IImporter::LoadExport<USubsurfaceProfile>(const TSharedPtr<FJsonObject> *,
                                          TObjectPtr<USubsurfaceProfile> &);
template void IImporter::LoadExport<UTexture>(const TSharedPtr<FJsonObject> *,
                                              TObjectPtr<UTexture> &);
template void IImporter::LoadExport<UMaterialParameterCollection>(
    const TSharedPtr<FJsonObject> *,
    TObjectPtr<UMaterialParameterCollection> &);
template void
IImporter::LoadExport<UAnimSequence>(const TSharedPtr<FJsonObject> *,
                                     TObjectPtr<UAnimSequence> &);
template void IImporter::LoadExport<USoundWave>(const TSharedPtr<FJsonObject> *,
                                                TObjectPtr<USoundWave> &);
template void IImporter::LoadExport<UObject>(const TSharedPtr<FJsonObject> *,
                                             TObjectPtr<UObject> &);
template void IImporter::LoadExport<UMaterialFunctionInterface>(
    const TSharedPtr<FJsonObject> *, TObjectPtr<UMaterialFunctionInterface> &);
template void IImporter::LoadExport<USoundNode>(const TSharedPtr<FJsonObject> *,
                                                TObjectPtr<USoundNode> &);
template void IImporter::LoadExport<UFontFace>(const TSharedPtr<FJsonObject> *,
                                               TObjectPtr<UFontFace> &);

template <typename T>
void IImporter::LoadExport(const TSharedPtr<FJsonObject> *PackageIndex,
                           TObjectPtr<T> &Object) {
  if (PackageIndex == nullptr || !PackageIndex->IsValid() ||
      PackageIndex->Get() == nullptr) {
    Object = nullptr;
    return;
  }

  if (!PackageIndex->Get()->HasField(TEXT("ObjectName")) ||
      !PackageIndex->Get()->HasField(TEXT("ObjectPath"))) {
    Object = nullptr;
    return;
  }

  FString ObjectType, ObjectName, ObjectPath, Outer;
  PackageIndex->Get()
      ->GetStringField(TEXT("ObjectName"))
      .Split("'", &ObjectType, &ObjectName);

  ObjectPath = PackageIndex->Get()->GetStringField(TEXT("ObjectPath"));
  ObjectPath.Split(".", &ObjectPath, nullptr);

  const UJsonAsAssetSettings *Settings = GetSettings();
  if (Settings == nullptr) {
    Object = nullptr;
    return;
  }

  if (!Settings->AssetSettings.ProjectName.IsEmpty()) {
    ObjectPath = ObjectPath.Replace(
        *(Settings->AssetSettings.ProjectName + "/Content/"), TEXT("/Game/"));
    ObjectPath = ObjectPath.Replace(
        *(Settings->AssetSettings.ProjectName + "/Plugins"), TEXT(""));
    ObjectPath = ObjectPath.Replace(TEXT("/Content/"), TEXT("/"));
  }

  ObjectPath = ObjectPath.Replace(TEXT("Engine/Content"), TEXT("/Engine"));
  ObjectName = ObjectName.Replace(TEXT("'"), TEXT(""));

  if (ObjectName.Contains(".")) {
    ObjectName.Split(".", nullptr, &ObjectName);
  }

  if (ObjectName.Contains(".")) {
    ObjectName.Split(".", &Outer, &ObjectName);
  }

  if (ObjectName.IsEmpty() || ObjectName == TEXT("None")) {
    Object = nullptr;
    return;
  }

  if (!ObjectPath.StartsWith(TEXT("/"))) {
    ObjectPath = "/" + ObjectPath;
  }

  FJRedirects::Redirect(ObjectPath);

  TObjectPtr<T> LoadedObject = nullptr;

  /* Try to load object using the object path and the object name combined */
  /* Subobject references (for example blueprint components or material
   * expressions) are resolved later, and forcing them through StaticLoadObject
   * can recurse into CoreUObject while the owning asset is still being built.
   */
  const bool bHasSubobjectReference =
      !Outer.IsEmpty() || ObjectName.Contains(TEXT(":"));
  if constexpr (!std::is_same_v<T, UObject>) {
    if (!ObjectPath.IsEmpty() && !ObjectName.IsEmpty() &&
        ObjectPath != TEXT("None") && ObjectName != TEXT("None") &&
        !bHasSubobjectReference) {
      const FString CandidatePath = ObjectPath + TEXT(".") + ObjectName;
      if (FPackageName::IsValidObjectPath(CandidatePath)) {
        LoadedObject = Cast<T>(
            StaticLoadObject(T::StaticClass(), nullptr, *CandidatePath));
      }
    }
  }

  if (!LoadedObject) {
    FString NewObjectPath;
    FString ObjectFileName;
    {
      ObjectPath.Split("/", &NewObjectPath, &ObjectFileName,
                       ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    }

    NewObjectPath = NewObjectPath + "/" + ObjectName;

    if constexpr (!std::is_same_v<T, UObject>) {
      if (!bHasSubobjectReference && !ObjectPath.IsEmpty() &&
          ObjectPath != TEXT("None") && ObjectFileName != ObjectName &&
          !NewObjectPath.IsEmpty() && !ObjectName.IsEmpty() &&
          NewObjectPath != TEXT("None")) {
        const FString CandidatePath = NewObjectPath + TEXT(".") + ObjectName;
        if (FPackageName::IsValidObjectPath(CandidatePath)) {
          LoadedObject = Cast<T>(
              StaticLoadObject(T::StaticClass(), nullptr, *CandidatePath));
        }
      }
    }
  }

  if (UObject *Parent = GetParent(); IsValid(Parent)) {
    if (!Outer.IsEmpty() && Parent->IsA(AActor::StaticClass())) {
      const AActor *NewLoadedObject = Cast<AActor>(Parent);
      auto Components = NewLoadedObject->GetComponents();

      for (UActorComponent *Component : Components) {
        if (ObjectName == Component->GetName()) {
          if constexpr (TIsDerivedFrom<T, UActorComponent>::IsDerived ||
                        TIsDerivedFrom<UActorComponent, T>::IsDerived) {
            LoadedObject = Cast<T>(Component);
          }
        }
      }
    }
  }

  /* Material Expression case */
  if constexpr (!std::is_same_v<T, UObject>) {
    if (!LoadedObject && !ObjectPath.IsEmpty() && ObjectPath != TEXT("None") &&
        ObjectName.Contains("MaterialExpression")) {
      FString SplitObjectName;
      ObjectPath.Split("/", nullptr, &SplitObjectName, ESearchCase::IgnoreCase,
                       ESearchDir::FromEnd);
      LoadedObject = Cast<T>(StaticLoadObject(
          T::StaticClass(), nullptr,
          *(ObjectPath + "." + SplitObjectName + ":" + ObjectName)));
    }
  }

  Object = LoadedObject;

  if (!Object && GetObjectSerializer() != nullptr &&
      GetPropertySerializer() != nullptr &&
      GetPropertySerializer()->ExportsContainer != nullptr) {
    const FUObjectExport *Export =
        GetPropertySerializer()->ExportsContainer->Find(ObjectName);

    if (Export && Export->IsJsonAndObjectValid() && Export->Object != nullptr &&
        Export->Object->IsA(T::StaticClass())) {
      Object = TObjectPtr<T>(Cast<T>(Export->Object));
    }
  }

  /* If object is still null, send off to Cloud to download */
  if (!Object) {
    Object = DownloadWrapper(LoadedObject, ObjectType, ObjectName, ObjectPath);
  }
}

template TArray<TObjectPtr<UCurveLinearColor>>
IImporter::LoadExport<UCurveLinearColor>(const TArray<TSharedPtr<FJsonValue>> &,
                                         TArray<TObjectPtr<UCurveLinearColor>>);

template <typename T>
TArray<TObjectPtr<T>>
IImporter::LoadExport(const TArray<TSharedPtr<FJsonValue>> &PackageArray,
                      TArray<TObjectPtr<T>> Array) {
  for (const TSharedPtr<FJsonValue> &ArrayElement : PackageArray) {
    if (!ArrayElement.IsValid() || ArrayElement->Type != EJson::Object ||
        !ArrayElement->AsObject().IsValid()) {
      UE_LOG(
          LogJsonAsAsset, Warning,
          TEXT("Skipping malformed export array element while loading '%s'."),
          *T::StaticClass()->GetName());
      continue;
    }

    const TSharedPtr<FJsonObject> ObjectPtr = ArrayElement->AsObject();
    TObjectPtr<T> Out;

    LoadExport<T>(&ObjectPtr, Out);

    Array.Add(Out);
  }

  return Array;
}

void IImporter::Save(const UObject *Asset) const {
  const UJsonAsAssetSettings *Settings = GetSettings();
  if (Settings == nullptr) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("IImporter::Save: settings are unavailable."));
    return;
  }

  /* Ensure the package is valid before proceeding */
  if (GetPackage() == nullptr) {
    UE_LOG(LogJsonAsAsset, Error, TEXT("IImporter::Save: Package is null"));
    return;
  }

  /* Hard fail-safe:
   * SavePackage can crash the editor for partially reconstructed assets
   * (notably mesh-backed imports) and those crashes are not recoverable here.
   * Keep imports non-fatal: mark dirty and require explicit user save.
   */
  if (!Settings->AssetSettings.SaveAssets) {
    return;
  }

  if (Asset != nullptr) {
    UE_LOG(LogJsonAsAsset, Warning,
           TEXT("Auto-save skipped for '%s' as a crash-safety guard. Asset "
                "was imported and marked dirty; save manually from editor."),
           *Asset->GetName());
  } else {
    UE_LOG(LogJsonAsAsset, Warning,
           TEXT("Auto-save skipped as a crash-safety guard. Imported package "
                "was marked dirty; save manually from editor."));
  }
}

bool IImporter::OnAssetCreation(UObject *Asset) const {
  if (Asset == nullptr || !IsValid(Asset)) {
    UE_LOG(
        LogJsonAsAsset, Error,
        TEXT("IImporter::OnAssetCreation failed: Asset is null or invalid."));
    return false;
  }

  UPackage *AssetPackage = GetPackage();
  if (AssetPackage == nullptr || !IsValid(AssetPackage)) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("IImporter::OnAssetCreation failed for '%s': package is null "
                "or invalid."),
           *Asset->GetName());
    return false;
  }

  const bool Synced = HandleAssetCreation(Asset, AssetPackage);
  if (Synced) {
    Save(Asset);
  }

  return Synced;
}

FUObjectExportContainer *IImporter::GetExportContainer() const {
  return GetObjectSerializer()->GetPropertySerializer()->ExportsContainer;
}
