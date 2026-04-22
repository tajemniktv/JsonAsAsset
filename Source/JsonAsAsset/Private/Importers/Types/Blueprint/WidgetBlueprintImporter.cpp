/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Blueprint/WidgetBlueprintImporter.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Engine/EngineUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Utilities/BlueprintUtilities.h"
#include "Utilities/JsonUtilities.h"

namespace
{
const FJBlueprintImportSettings& GetBlueprintImportSettings()
{
	return GetSettings()->BlueprintImport;
}

bool ShouldCompileImmediately()
{
	return GetBlueprintImportSettings().CompilePolicy == EJBlueprintCompilePolicy::Immediate;
}

bool UseWidgetCompatibilityFallback()
{
	const UJsonAsAssetSettings* Settings = GetSettings();
	return Settings && Settings->CompatibilityFallback.WidgetBlueprintGeneratedClass;
}

UClass* ResolveWidgetClass(const FUObjectExport* WidgetExport)
{
	if (!WidgetExport || !WidgetExport->JsonObject.IsValid() || !WidgetExport->JsonObject->HasField(TEXT("Class"))) {
		return nullptr;
	}

	FString ClassName = WidgetExport->JsonObject->GetStringField(TEXT("Class"));
	if (!ClassName.StartsWith(TEXT("UScriptClass'"))) {
		return nullptr;
	}

	ClassName = ClassName.Replace(TEXT("UScriptClass'"), TEXT("")).Replace(TEXT("'"), TEXT(""));
	UClass* ResolvedClass = LoadClassFromPath(ClassName, TEXT("/Script/UMG"));

	if (!ResolvedClass) {
		const UJsonAsAssetSettings* Settings = GetSettings();
		if (Settings && !Settings->AssetSettings.ProjectName.IsEmpty()) {
			ResolvedClass = LoadClassFromPath(ClassName, TEXT("/Script/") + Settings->AssetSettings.ProjectName);
		}
	}

	return ResolvedClass;
}
}

UObject* IWidgetBlueprintImporter::CreateAsset(UObject* CreatedAsset)
{
	if (CreatedAsset) {
		return IImporter::CreateAsset(CreatedAsset);
	}

	const TSharedPtr<FJsonObject> SuperStruct = GetAssetData()->GetObjectField(TEXT("SuperStruct"));
	UClass* ParentClass = LoadClass(SuperStruct);
	if (!ParentClass) {
		return nullptr;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		GetPackage(),
		*GetAssetName(),
		BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass()
	);

	return IImporter::CreateAsset(Blueprint);
}

bool IWidgetBlueprintImporter::Import()
{
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(CreateAsset());
	if (!WidgetBlueprint || !WidgetBlueprint->GeneratedClass || !WidgetBlueprint->WidgetTree) {
		return false;
	}

	if (GetAssetData()->HasField(TEXT("WidgetTree"))) {
		FUObjectExport* WidgetTreeExport = AssetContainer->GetExportByObjectPath(GetAssetData()->GetObjectField(TEXT("WidgetTree")));
		if (WidgetTreeExport && WidgetTreeExport->IsJsonValid() && WidgetTreeExport->Has(TEXT("Properties"))) {
			const TSharedPtr<FJsonObject> WidgetTreeProps = WidgetTreeExport->GetProperties();
			if (WidgetTreeProps.IsValid() && WidgetTreeProps->HasField(TEXT("RootWidget"))) {
				FUObjectExport* RootWidgetExport = AssetContainer->GetExportByObjectPath(WidgetTreeProps->GetObjectField(TEXT("RootWidget")));
				if (RootWidgetExport && RootWidgetExport->IsJsonValid()) {
					UClass* RootClass = ResolveWidgetClass(RootWidgetExport);
					if (!RootClass && RootWidgetExport->Has(TEXT("Template"))) {
						TObjectPtr<UObject> ParentWidgetObject;
						TSharedPtr<FJsonObject> TemplateObject = RootWidgetExport->GetObject(TEXT("Template")).JsonObject;
						if (TemplateObject.IsValid()) {
							LoadExport(&TemplateObject, ParentWidgetObject);
							if (UWidgetBlueprintGeneratedClass* WidgetBpClass = Cast<UWidgetBlueprintGeneratedClass>(ParentWidgetObject.Get())) {
								RootClass = WidgetBpClass;
							}
						}
					}

					if (RootClass && RootClass->IsChildOf(UWidget::StaticClass())) {
						UWidget* RootWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(RootClass, RootWidgetExport->GetName());
						WidgetBlueprint->WidgetTree->RootWidget = RootWidget;

						if (UPanelWidget* RootPanel = Cast<UPanelWidget>(RootWidget)) {
							HandlePanelSlots(WidgetBlueprint, RootWidgetExport, RootPanel);
						}

						GetObjectSerializer()->DeserializeObjectProperties(
							RemovePropertiesShared(RootWidgetExport->GetProperties(), { TEXT("Slot"), TEXT("Slots") }),
							RootWidget
						);
					} else if (UseWidgetCompatibilityFallback()) {
						UCanvasPanel* RootWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), RootWidgetExport->GetName());
						WidgetBlueprint->WidgetTree->RootWidget = RootWidget;
						HandlePanelSlots(WidgetBlueprint, RootWidgetExport, RootWidget);
					}
				}
			}
		}
	}

	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(AssetContainer, GetAssetDataAsValue());
	if (ClassDefaultObjectExport && ClassDefaultObjectExport->IsJsonValid() && ClassDefaultObjectExport->GetProperties().IsValid()) {
		UObject* ClassDefaultObject = WidgetBlueprint->GeneratedClass->GetDefaultObject();
		GetObjectSerializer()->DeserializeObjectProperties(
			RemovePropertiesShared(ClassDefaultObjectExport->GetProperties(), {
				TEXT("UberGraphFrame"),
				TEXT("WidgetTree")
			}),
			ClassDefaultObject
		);
	}

	if (ShouldCompileImmediately()) {
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::None);
	} else {
		UE_LOG(LogJsonAsAsset, Warning, TEXT("Skipping immediate compile for Widget Blueprint '%s' due to BlueprintImport.CompilePolicy=%s"),
			*GetAssetName(),
			*StaticEnum<EJBlueprintCompilePolicy>()->GetNameStringByValue(static_cast<int64>(GetBlueprintImportSettings().CompilePolicy)));
	}

	return OnAssetCreation(WidgetBlueprint);
}

void IWidgetBlueprintImporter::HandlePanelSlots(UWidgetBlueprint* WidgetBlueprint, FUObjectExport* PanelExport, UPanelWidget* Panel)
{
	if (!WidgetBlueprint || !PanelExport || !PanelExport->IsJsonValid() || !PanelExport->HasProperty(TEXT("Slots")) || !Panel) {
		return;
	}

	const TArray<TSharedPtr<FJsonValue>> Slots = PanelExport->GetProperties()->GetArrayField(TEXT("Slots"));
	for (const TSharedPtr<FJsonValue>& SlotValue : Slots) {
		const TSharedPtr<FJsonObject> SlotObjectPath = SlotValue->AsObject();
		if (!SlotObjectPath.IsValid()) {
			continue;
		}

		FUObjectExport* PanelSlotExport = AssetContainer->GetExportByObjectPath(SlotObjectPath);
		if (!PanelSlotExport || !PanelSlotExport->IsJsonValid() || !PanelSlotExport->HasProperty(TEXT("Content"))) {
			continue;
		}

		FUObjectExport* SlotContentExport = AssetContainer->GetExportByObjectPath(PanelSlotExport->GetProperties()->GetObjectField(TEXT("Content")));
		if (!SlotContentExport || !SlotContentExport->IsJsonValid()) {
			continue;
		}

		UClass* WidgetClass = ResolveWidgetClass(SlotContentExport);
		if (!WidgetClass && SlotContentExport->Has(TEXT("Template"))) {
			TObjectPtr<UObject> ParentWidgetObject;
			TSharedPtr<FJsonObject> TemplateObject = SlotContentExport->GetObject(TEXT("Template")).JsonObject;
			if (TemplateObject.IsValid()) {
				LoadExport(&TemplateObject, ParentWidgetObject);
				if (UWidgetBlueprintGeneratedClass* WidgetBpClass = Cast<UWidgetBlueprintGeneratedClass>(ParentWidgetObject.Get())) {
					WidgetClass = WidgetBpClass;
				}
			}
		}

		if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass())) {
			continue;
		}

		UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, SlotContentExport->GetName());
		if (!NewWidget) {
			continue;
		}

		if (UPanelWidget* ChildPanel = Cast<UPanelWidget>(NewWidget)) {
			HandlePanelSlots(WidgetBlueprint, SlotContentExport, ChildPanel);
		} else {
			GetObjectSerializer()->DeserializeObjectProperties(
				RemovePropertiesShared(SlotContentExport->GetProperties(), { TEXT("Slot") }),
				NewWidget
			);
		}

		UPanelSlot* AddedSlot = Panel->AddChild(NewWidget);
		if (!AddedSlot) {
			continue;
		}

		const FString SlotType = PanelSlotExport->GetType().ToString();
		if (SlotType == TEXT("CanvasPanelSlot")) {
			if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(AddedSlot)) {
				GetObjectSerializer()->DeserializeObjectProperties(
					KeepPropertiesShared(PanelSlotExport->GetProperties(), {
						TEXT("LayoutData"),
						TEXT("ZOrder"),
						TEXT("Anchors"),
						TEXT("Offsets"),
						TEXT("Alignment"),
						TEXT("AutoSize")
					}),
					CanvasSlot
				);
			}
		}
	}
}
