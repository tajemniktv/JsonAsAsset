/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintImporter.h"

#include "KismetCompilerModule.h"
#include "MovieScene.h"
#include "WidgetBlueprint.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MVVM/ViewModels/ObjectBindingModel.h"
#include "Utilities/BlueprintUtilities.h"

UObject* IBlueprintImporter::CreateAsset(UObject* CreatedAsset) {
	UPackage* AssetPackage = GetPackage();
	if (!AssetPackage || !IsValid(AssetPackage)) {
		UE_LOG(LogJsonAsAsset, Error,
		       TEXT("Blueprint CreateAsset failed: invalid package for '%s'."),
		       *GetAssetName());
		return nullptr;
	}

	UClass* Class = GetAssetClass();
	
	if (!Class) {
		AppendNotification(
			FText::FromString("Failed to Resolve Parent Class"),
			FText::FromString("The Blueprint's parent class could not be found or loaded. Verify that the class is defined and available at import time."),
			2.0f,
			SNotificationItem::CS_Fail,
			true,
			350.0f
		);
		
		return nullptr;
	}
	
	/* Find the blueprint class and generated class */
	UClass* BlueprintClass = nullptr, *GeneratedClass = nullptr;
	
	FModuleManager::LoadModuleChecked<IKismetCompilerInterface>
		("KismetCompiler")
			.GetBlueprintTypesForClass(
				Class,
				BlueprintClass,
				GeneratedClass
			);

	if (!BlueprintClass || !GeneratedClass) {
		UE_LOG(
		    LogJsonAsAsset, Error,
		    TEXT(
		        "Blueprint CreateAsset failed for '%s': blueprint classes could not be resolved (BlueprintClass=%s, GeneratedClass=%s)."),
		    *GetAssetName(),
		    BlueprintClass ? *BlueprintClass->GetName() : TEXT("null"),
		    GeneratedClass ? *GeneratedClass->GetName() : TEXT("null"));
		return nullptr;
	}

	/* Propagate blueprint defaults if it already exists */
	const FString ExistingObjectPath = AssetPackage->GetPathName() + TEXT(".") + GetAssetName();
	if (const UBlueprint* ExistingBlueprint = FindObject<UBlueprint>(nullptr, *ExistingObjectPath)) {
		UBlueprintGeneratedClass* BlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(ExistingBlueprint->GeneratedClass);
		if (!BlueprintGeneratedClass) {
			UE_LOG(
			    LogJsonAsAsset, Warning,
			    TEXT("Existing blueprint '%s' has no generated class; skipping default propagation."),
			    *ExistingObjectPath);
		} else {
			FBlueprintEditorUtils::PropagateParentBlueprintDefaults(BlueprintGeneratedClass);
		}

		/* Return GeneratedClass instead of UBlueprint* */
		return IImporter::CreateAsset(BlueprintGeneratedClass ? static_cast<UObject*>(BlueprintGeneratedClass)
		                                                     : const_cast<UBlueprint*>(ExistingBlueprint));
	}

	const UBlueprint* CreatedBlueprint = FKismetEditorUtilities::CreateBlueprint(
		Class,
		AssetPackage,
		FName(*GetAssetName()),
		GetBlueprintType(Class),
		BlueprintClass,
		GeneratedClass
	);

	if (!CreatedBlueprint) return nullptr;

	/* Return GeneratedClass instead of UBlueprint* */
	return IImporter::CreateAsset(CreatedBlueprint->GeneratedClass);
}

bool IBlueprintImporter::Import() {
	const UBlueprintGeneratedClass* BlueprintGeneratedClass = Create<UBlueprintGeneratedClass>();
	if (!BlueprintGeneratedClass) return false;

	/* Update Blueprint Reference for sub functions */
	Blueprint = UBlueprint::GetBlueprintFromClass(BlueprintGeneratedClass);
	if (!Blueprint) return false;

	/* Deserialize Generated Class (blueprint defaults) */
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass || !GeneratedClass->GetDefaultObject()) {
		UE_LOG(LogJsonAsAsset, Error,
		       TEXT("Blueprint import failed for '%s': generated class/default object is invalid."),
		       *GetAssetName());
		return false;
	}
	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());
	if (!ClassDefaultObjectExport || !ClassDefaultObjectExport->GetProperties().IsValid()) {
		UE_LOG(LogJsonAsAsset, Warning,
		       TEXT("Blueprint import for '%s': class default export/properties missing; skipping CDO deserialize."),
		       *GetAssetName());
		return OnAssetCreation(Blueprint);
	}
	ClassDefaultObjectExport->Object = GeneratedClass;

	GetObjectSerializer()->DeserializeObjectProperties(ClassDefaultObjectExport->GetProperties(), GeneratedClass->GetDefaultObject());

	/* Experimental (for now) spawning */
	GetObjectSerializer()->bUseExperimentalSpawning = true;

	ConstructScript();
	ConstructWidgetTree();

	return OnAssetCreation(Blueprint);
}

void IBlueprintImporter::ConstructScript() const {
	if (!GetAssetDataAsValue().Has("SimpleConstructionScript")) return;
	
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);

	/* Destroy Construction Script */
	if (USimpleConstructionScript* PreviousSimpleConstructionScript = GeneratedClass->SimpleConstructionScript; PreviousSimpleConstructionScript != nullptr) {
		MoveToTransientPackagesAndRename({
			PreviousSimpleConstructionScript,
			Blueprint->SimpleConstructionScript
		});
	}

	FUObjectExport* Export = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("SimpleConstructionScript"));

	/* Spawn the new Construction Script */
	USimpleConstructionScript* SimpleConstructionScript =
		Cast<USimpleConstructionScript>(
			GetObjectSerializer()->SpawnExport(Export)
		);

	/* Update SimpleConstructionScript on the Blueprint */
	Blueprint->SimpleConstructionScript = SimpleConstructionScript;
	GeneratedClass->SimpleConstructionScript = SimpleConstructionScript;

	/* Engine Ensures */
	SimpleConstructionScript->FixupRootNodeParentReferences();
	SimpleConstructionScript->ValidateSceneRootNodes();
}

class UWidgetTreeAccessor final : public UWidgetTree {
public:
	TArray<TObjectPtr<UWidget>> GetWidgets() {
		return AllWidgets;
	}
};

void IBlueprintImporter::ConstructWidgetTree() {
	if (!GetAssetDataAsValue().Has("WidgetTree")) return;

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
	if (!WidgetBlueprint || !IsValid(WidgetBlueprint)) {
		UE_LOG(LogJsonAsAsset, Warning,
		       TEXT("ConstructWidgetTree skipped for '%s': WidgetBlueprint is invalid."),
		       *GetAssetName());
		return;
	}

	if (!WidgetBlueprint->WidgetTree || !IsValid(WidgetBlueprint->WidgetTree)) {
		UE_LOG(LogJsonAsAsset, Warning,
		       TEXT("ConstructWidgetTree skipped for '%s': WidgetTree is invalid."),
		       *GetAssetName());
		return;
	}
	
	for (UWidget* Widget : Cast<UWidgetTreeAccessor>(WidgetBlueprint->WidgetTree)->GetWidgets()) {
		MoveToTransientPackageAndRename(Widget);
	}

	WidgetBlueprint->WidgetTree->PostLoad();

	for (UWidgetAnimation* WidgetAnimation : WidgetBlueprint->Animations) {
		MoveToTransientPackageAndRename(WidgetAnimation);
	}

	WidgetBlueprint->Animations.Empty();
	
	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());
	if (!ClassDefaultObjectExport) {
		UE_LOG(LogJsonAsAsset, Warning,
		       TEXT("ConstructWidgetTree for '%s': missing class default object export."),
		       *GetAssetName());
		return;
	}
	ClassDefaultObjectExport->Object = WidgetBlueprint;
	SetAsset(WidgetBlueprint);

	if (WidgetBlueprint->WidgetTree->RootWidget) {
		MoveToTransientPackageAndRename(WidgetBlueprint->WidgetTree->RootWidget);
	}
	WidgetBlueprint->WidgetTree->RootWidget = nullptr;
	
	FUObjectExport* Export = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("WidgetTree"));
	if (!Export) {
		UE_LOG(LogJsonAsAsset, Warning,
		       TEXT("ConstructWidgetTree for '%s': missing WidgetTree export."),
		       *GetAssetName());
		return;
	}
	Export->Object = WidgetBlueprint->WidgetTree;
	GetObjectSerializer()->SpawnExport(Export, true);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	GetContainer()->ExportsLoop(GetAssetDataAsValue().GetArray("Animations"), [this, WidgetBlueprint](FUObjectExport* DirectExport) {
		if (UObject* Object = GetObjectSerializer()->SpawnExport(DirectExport)) {
			UWidgetAnimation* WidgetAnimation = Cast<UWidgetAnimation>(Object);
			if (!WidgetAnimation || !IsValid(WidgetAnimation)) {
				return;
			}
		
			WidgetBlueprint->Animations.Add(WidgetAnimation);
			if (!WidgetAnimation->MovieScene || !IsValid(WidgetAnimation->MovieScene)) {
				return;
			}

			for (int32 Index = 0; Index < WidgetAnimation->MovieScene->GetPossessableCount(); ++Index) {
				FMovieScenePossessable& Possessable = WidgetAnimation->MovieScene->GetPossessable(Index);

				TArray<UWidget*> Widgets;
				WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);

				for (UWidget* Widget : Widgets)
				{
					if (Widget->GetName() == Possessable.GetName())
					{
						Possessable.SetPossessedObjectClass(Widget->GetClass());
					}
				}
			}
			
			for (const FMovieSceneBinding& Binding : WidgetAnimation->MovieScene->GetBindings())
			{
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					Track->Modify();
					Track->MarkAsChanged();

					if (UMovieSceneWidgetMaterialTrack* MaterialTrack = Cast<UMovieSceneWidgetMaterialTrack>(Track))
					{
						const TArray<FName>& BrushPropertyPath = MaterialTrack->GetBrushPropertyNamePath();
						if (BrushPropertyPath.Num() > 0) {
							MaterialTrack->SetDisplayName(FText::FromString(BrushPropertyPath[0].ToString()));
						}
					}
				}
			}
		}
	});
}
