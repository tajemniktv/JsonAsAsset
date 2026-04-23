/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Blueprint/WidgetBlueprintImporter.h"

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Engine/EngineUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Settings/Runtime.h"
#include "Utilities/BlueprintUtilities.h"
#include "Utilities/JsonUtilities.h"
#include "WidgetBlueprint.h"

namespace {
const FJBlueprintImportSettings &GetBlueprintImportSettings() {
  return GetSettings()->BlueprintImport;
}

bool IsStrictBlueprintImport() {
  return GetBlueprintImportSettings().StrictMode;
}

bool ShouldCompileImmediately() {
  return GetBlueprintImportSettings().CompilePolicy ==
         EJBlueprintCompilePolicy::Immediate;
}

bool UseWidgetCompatibilityFallback() {
  const UJsonAsAssetSettings *Settings = GetSettings();
  if (Settings && Settings->CompatibilityFallback.WidgetBlueprintGeneratedClass) {
    return true;
  }

  return GJsonAsAssetRuntime.IsBetterMartPresetActive();
}

bool HasPendingLoadFlags(const UObject *Object) {
  if (!Object) {
    return false;
  }

  return Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad |
                             RF_ClassDefaultObject);
}

bool IsWidgetBlueprintSafeToReuse(const UWidgetBlueprint *Blueprint) {
  if (!Blueprint || !Blueprint->GeneratedClass || !Blueprint->WidgetTree) {
    return false;
  }

  return !HasPendingLoadFlags(Blueprint) &&
         !HasPendingLoadFlags(Blueprint->GeneratedClass) &&
         !HasPendingLoadFlags(Blueprint->WidgetTree);
}

UClass *ResolveWidgetClass(const FUObjectExport *WidgetExport) {
  if (!WidgetExport || !WidgetExport->JsonObject.IsValid() ||
      !WidgetExport->JsonObject->HasField(TEXT("Class"))) {
    return nullptr;
  }

  FString ClassName = WidgetExport->JsonObject->GetStringField(TEXT("Class"));
  if (!ClassName.StartsWith(TEXT("UScriptClass'"))) {
    return nullptr;
  }

  ClassName = ClassName.Replace(TEXT("UScriptClass'"), TEXT(""))
                  .Replace(TEXT("'"), TEXT(""));
  UClass *ResolvedClass = LoadClassFromPath(ClassName, TEXT("/Script/UMG"));

  if (!ResolvedClass) {
    const UJsonAsAssetSettings *Settings = GetSettings();
    if (Settings && !Settings->AssetSettings.ProjectName.IsEmpty()) {
      ResolvedClass = LoadClassFromPath(
          ClassName, TEXT("/Script/") + Settings->AssetSettings.ProjectName);
    }
  }

  return ResolvedClass;
}
} // namespace

UObject *IWidgetBlueprintImporter::CreateAsset(UObject *CreatedAsset) {
  if (UWidgetBlueprint *ExistingAsset = Cast<UWidgetBlueprint>(CreatedAsset)) {
    return IImporter::CreateAsset(ExistingAsset);
  }

  if (!GetPackage()) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("Widget Blueprint '%s' has no target package."), *GetAssetName());
    return nullptr;
  }

  UBlueprint *ExistingBlueprint =
      FindObject<UBlueprint>(GetPackage(), *GetAssetName());
  if (!ExistingBlueprint) {
    const FString ExistingObjectPath =
        GetPackage()->GetPathName() + TEXT(".") + GetAssetName();
    ExistingBlueprint = LoadObject<UBlueprint>(nullptr, *ExistingObjectPath);
  }

  const EJBlueprintReimportPolicy Policy =
      GetBlueprintImportSettings().ReimportPolicy;
  if (ExistingBlueprint &&
      Policy == EJBlueprintReimportPolicy::AlwaysRecreate) {
    MoveToTransientPackageAndRename(ExistingBlueprint);
    ExistingBlueprint = nullptr;
  }

  if (ExistingBlueprint) {
    UWidgetBlueprint *ExistingWidgetBlueprint =
        Cast<UWidgetBlueprint>(ExistingBlueprint);
    if (!ExistingWidgetBlueprint) {
      UE_LOG(LogJsonAsAsset, Error,
             TEXT("Cannot create Widget Blueprint '%s': existing object is not a UWidgetBlueprint."),
             *GetAssetName());
      return nullptr;
    }

    if (Policy == EJBlueprintReimportPolicy::RecreateInvalid &&
        !IsWidgetBlueprintSafeToReuse(ExistingWidgetBlueprint)) {
      MoveToTransientPackageAndRename(ExistingWidgetBlueprint);
      ExistingWidgetBlueprint = nullptr;
    } else if (Policy == EJBlueprintReimportPolicy::ReuseValid &&
               !IsWidgetBlueprintSafeToReuse(ExistingWidgetBlueprint)) {
      if (IsStrictBlueprintImport()) {
        UE_LOG(LogJsonAsAsset, Error,
               TEXT("ReuseValid policy prevented recreation of invalid Widget Blueprint '%s'."),
               *GetAssetName());
        return nullptr;
      }

      MoveToTransientPackageAndRename(ExistingWidgetBlueprint);
      ExistingWidgetBlueprint = nullptr;
    }

    if (ExistingWidgetBlueprint) {
      return IImporter::CreateAsset(ExistingWidgetBlueprint);
    }
  }

  if (!GetAssetData()->HasField(TEXT("SuperStruct"))) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("Widget Blueprint '%s' is missing SuperStruct metadata."),
           *GetAssetName());
    return nullptr;
  }

  const TSharedPtr<FJsonObject> SuperStruct =
      GetAssetData()->GetObjectField(TEXT("SuperStruct"));
  UClass *ParentClass = LoadClass(SuperStruct);
  if (!ParentClass) {
    return nullptr;
  }

  UBlueprint *Blueprint = FKismetEditorUtilities::CreateBlueprint(
      ParentClass, GetPackage(), *GetAssetName(), BPTYPE_Normal,
      UWidgetBlueprint::StaticClass(),
      UWidgetBlueprintGeneratedClass::StaticClass());

  return IImporter::CreateAsset(Blueprint);
}

bool IWidgetBlueprintImporter::Import() {
  UWidgetBlueprint *WidgetBlueprint = Cast<UWidgetBlueprint>(CreateAsset());
  if (!WidgetBlueprint || !WidgetBlueprint->GeneratedClass ||
      !WidgetBlueprint->WidgetTree) {
    return false;
  }

  bool bRootWidgetResolved = false;
  if (GetAssetData()->HasField(TEXT("WidgetTree"))) {
    FUObjectExport *WidgetTreeExport = AssetContainer->GetExportByObjectPath(
        GetAssetData()->GetObjectField(TEXT("WidgetTree")));
    if (WidgetTreeExport && WidgetTreeExport->IsJsonValid() &&
        WidgetTreeExport->Has(TEXT("Properties"))) {
      const TSharedPtr<FJsonObject> WidgetTreeProps =
          WidgetTreeExport->GetProperties();
      if (WidgetTreeProps.IsValid() &&
          WidgetTreeProps->HasField(TEXT("RootWidget"))) {
        FUObjectExport *RootWidgetExport =
            AssetContainer->GetExportByObjectPath(
                WidgetTreeProps->GetObjectField(TEXT("RootWidget")));
        if (RootWidgetExport && RootWidgetExport->IsJsonValid()) {
          UClass *RootClass = ResolveWidgetClass(RootWidgetExport);
          if (!RootClass && RootWidgetExport->Has(TEXT("Template"))) {
            TObjectPtr<UObject> ParentWidgetObject;
            TSharedPtr<FJsonObject> TemplateObject =
                RootWidgetExport->GetObject(TEXT("Template")).JsonObject;
            if (TemplateObject.IsValid()) {
              LoadExport(&TemplateObject, ParentWidgetObject);
              if (UWidgetBlueprintGeneratedClass *WidgetBpClass =
                      Cast<UWidgetBlueprintGeneratedClass>(
                          ParentWidgetObject.Get())) {
                RootClass = WidgetBpClass;
              }
            }
          }

          if (RootClass && RootClass->IsChildOf(UWidget::StaticClass())) {
            UWidget *RootWidget =
                WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(
                    RootClass, RootWidgetExport->GetName());
            WidgetBlueprint->WidgetTree->RootWidget = RootWidget;
            bRootWidgetResolved = RootWidget != nullptr;

            if (UPanelWidget *RootPanel = Cast<UPanelWidget>(RootWidget)) {
              HandlePanelSlots(WidgetBlueprint, RootWidgetExport, RootPanel);
            }

            GetObjectSerializer()->DeserializeObjectProperties(
                RemovePropertiesShared(RootWidgetExport->GetProperties(),
                                       {TEXT("Slot"), TEXT("Slots")}),
                RootWidget);
          } else if (UseWidgetCompatibilityFallback()) {
            UCanvasPanel *RootWidget =
                WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
                    UCanvasPanel::StaticClass(), RootWidgetExport->GetName());
            WidgetBlueprint->WidgetTree->RootWidget = RootWidget;
            bRootWidgetResolved = RootWidget != nullptr;
            HandlePanelSlots(WidgetBlueprint, RootWidgetExport, RootWidget);
          } else {
            UE_LOG(LogJsonAsAsset, Warning,
                   TEXT("Widget Blueprint '%s': failed to resolve root widget class and compatibility fallback is disabled."),
                   *GetAssetName());
          }
        }
      }
    }
  }

  if (!bRootWidgetResolved && IsStrictBlueprintImport()) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("Widget Blueprint '%s': strict mode enabled and root widget was not resolved."),
           *GetAssetName());
    return false;
  }

  FUObjectExport *ClassDefaultObjectExport =
      GetClassDefaultObject(AssetContainer, GetAssetDataAsValue());
  if (ClassDefaultObjectExport && ClassDefaultObjectExport->IsJsonValid() &&
      ClassDefaultObjectExport->GetProperties().IsValid()) {
    UObject *ClassDefaultObject =
        WidgetBlueprint->GeneratedClass->GetDefaultObject();
    GetObjectSerializer()->DeserializeObjectProperties(
        RemovePropertiesShared(ClassDefaultObjectExport->GetProperties(),
                               {TEXT("UberGraphFrame"), TEXT("WidgetTree")}),
        ClassDefaultObject);
  }

  if (ShouldCompileImmediately()) {
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint,
                                             EBlueprintCompileOptions::None);
  } else {
    UE_LOG(LogJsonAsAsset, Log,
           TEXT("Skipping immediate compile for Widget Blueprint '%s' due to "
                "BlueprintImport.CompilePolicy=%s"),
           *GetAssetName(),
           *StaticEnum<EJBlueprintCompilePolicy>()->GetNameStringByValue(
               static_cast<int64>(GetBlueprintImportSettings().CompilePolicy)));
  }

  return OnAssetCreation(WidgetBlueprint);
}

void IWidgetBlueprintImporter::HandlePanelSlots(
    UWidgetBlueprint *WidgetBlueprint, FUObjectExport *PanelExport,
    UPanelWidget *Panel) {
  if (!WidgetBlueprint || !PanelExport || !PanelExport->IsJsonValid() ||
      !PanelExport->HasProperty(TEXT("Slots")) || !Panel) {
    return;
  }

  const TSharedPtr<FJsonObject> PanelProperties = PanelExport->GetProperties();
  if (!PanelProperties.IsValid() ||
      !PanelProperties->HasTypedField<EJson::Array>(TEXT("Slots"))) {
    return;
  }

  const TArray<TSharedPtr<FJsonValue>> Slots =
      PanelProperties->GetArrayField(TEXT("Slots"));
  for (const TSharedPtr<FJsonValue> &SlotValue : Slots) {
    if (!SlotValue.IsValid() || SlotValue->Type != EJson::Object ||
        !SlotValue->AsObject().IsValid()) {
      continue;
    }

    const TSharedPtr<FJsonObject> SlotObjectPath = SlotValue->AsObject();
    if (!SlotObjectPath.IsValid()) {
      continue;
    }

    FUObjectExport *PanelSlotExport =
        AssetContainer->GetExportByObjectPath(SlotObjectPath);
    if (!PanelSlotExport || !PanelSlotExport->IsJsonValid() ||
        !PanelSlotExport->HasProperty(TEXT("Content"))) {
      continue;
    }

    FUObjectExport *SlotContentExport = AssetContainer->GetExportByObjectPath(
        PanelSlotExport->GetProperties()->GetObjectField(TEXT("Content")));
    if (!SlotContentExport || !SlotContentExport->IsJsonValid()) {
      continue;
    }

    UClass *WidgetClass = ResolveWidgetClass(SlotContentExport);
    if (!WidgetClass && SlotContentExport->Has(TEXT("Template"))) {
      TObjectPtr<UObject> ParentWidgetObject;
      TSharedPtr<FJsonObject> TemplateObject =
          SlotContentExport->GetObject(TEXT("Template")).JsonObject;
      if (TemplateObject.IsValid()) {
        LoadExport(&TemplateObject, ParentWidgetObject);
        if (UWidgetBlueprintGeneratedClass *WidgetBpClass =
                Cast<UWidgetBlueprintGeneratedClass>(
                    ParentWidgetObject.Get())) {
          WidgetClass = WidgetBpClass;
        }
      }
    }

    if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass())) {
      continue;
    }

    UWidget *NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(
        WidgetClass, SlotContentExport->GetName());
    if (!NewWidget) {
      continue;
    }

    if (UPanelWidget *ChildPanel = Cast<UPanelWidget>(NewWidget)) {
      HandlePanelSlots(WidgetBlueprint, SlotContentExport, ChildPanel);
    } else {
      GetObjectSerializer()->DeserializeObjectProperties(
          RemovePropertiesShared(SlotContentExport->GetProperties(),
                                 {TEXT("Slot")}),
          NewWidget);
    }

    UPanelSlot *AddedSlot = Panel->AddChild(NewWidget);
    if (!AddedSlot) {
      continue;
    }

    const FString SlotType = PanelSlotExport->GetType().ToString();
    if (SlotType == TEXT("CanvasPanelSlot")) {
      if (UCanvasPanelSlot *CanvasSlot = Cast<UCanvasPanelSlot>(AddedSlot)) {
        GetObjectSerializer()->DeserializeObjectProperties(
            KeepPropertiesShared(PanelSlotExport->GetProperties(),
                                 {TEXT("LayoutData"), TEXT("ZOrder"),
                                  TEXT("Anchors"), TEXT("Offsets"),
                                  TEXT("Alignment"), TEXT("AutoSize")}),
            CanvasSlot);
      }
    } else {
      GetObjectSerializer()->DeserializeObjectProperties(
          RemovePropertiesShared(PanelSlotExport->GetProperties(),
                                 {TEXT("Content"), TEXT("Parent"),
                                  TEXT("Slot"), TEXT("Slots")}),
          AddedSlot);
    }
  }
}
