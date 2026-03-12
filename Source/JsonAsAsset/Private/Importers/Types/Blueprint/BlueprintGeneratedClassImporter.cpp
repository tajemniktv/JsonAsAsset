/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintGeneratedClassImporter.h"

#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

#include "Importers/Types/Blueprint/Utilities/BlueprintUtilities.h"

#if WITH_EDITOR
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#endif

#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"

UObject* IBlueprintGeneratedClassImporter::CreateAsset(UObject* CreatedAsset) {
	const TSharedPtr<FJsonObject> SuperStruct = GetAssetData()->GetObjectField(TEXT("SuperStruct"));
	UClass* ParentClass = LoadClass(SuperStruct);
	if (!ParentClass) {
		TObjectPtr<UObject> Parent;
		LoadExport(&SuperStruct, Parent);
		if (!Parent.IsValid()) {
			return false;
		}
		ParentClass = LoadClass(SuperStruct);
	}
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, GetPackage(), *GetAssetName(), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	
	return IImporter::CreateAsset(Blueprint);
}

bool IBlueprintGeneratedClassImporter::Import() {
	UBlueprint* Blueprint = nullptr;
	Blueprint = FindObject<UBlueprint>(GetPackage(), *GetAssetName());
	if (!Blueprint) {
		Blueprint = Create<UBlueprint>();
		UClass* BlueprintClass = Blueprint->GeneratedClass;

		// If it inherit from an actor
		if (BlueprintClass->IsChildOf(AActor::StaticClass()))
		{
			const TSharedPtr<FJsonObject> Properties = GetAssetData();
			FUObjectExport SimpleConstructionScriptObjectExport = AssetContainer.GetExportByObjectPath(GetAssetData()->GetObjectField(TEXT("SimpleConstructionScript")));

			// Root
			USCS_Node* RootNode = Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode();
			const TArray<TSharedPtr<FJsonValue>> RootNodesObject = SimpleConstructionScriptObjectExport.GetProperties()->GetArrayField(TEXT("RootNodes"));
			HandleSimpleConstructionScript(Blueprint, RootNode, RootNodesObject, true);

			FUObjectExport InheritableComponentHandlerExport = AssetContainer.GetExportByObjectPath(GetAssetData()->GetObjectField(TEXT("InheritableComponentHandler")));
			if (InheritableComponentHandlerExport.IsValid()) {
				HandleInheritableComponentHandler(Blueprint, InheritableComponentHandlerExport);
			}
		}

		// Create the variables by looping the Children array an checking if it's a StrProperty, BoolProperty
		// Create them after creating the components to avoid them re-creating it
		CreateVariables(Blueprint, GetAssetName(), GetAssetData()->GetArrayField(TEXT("Children")));

		ReadFuncMap(Blueprint);

		FUObjectExport ClassDefaultObjectExport = AssetContainer.GetExportByObjectPath(GetAssetData()->GetObjectField(TEXT("ClassDefaultObject")));
		if (ClassDefaultObjectExport.GetProperties().IsValid()) {
			UObject* ClassDefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
			GetObjectSerializer()->SetExportForDeserialization(GetAssetExport(), ClassDefaultObject);
			GetObjectSerializer()->Parent = Blueprint->GeneratedClass->GetDefaultObject();
			GetObjectSerializer()->SetupExports(AssetContainer.JsonObjects);
			GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(ClassDefaultObjectExport.GetProperties(), {
				"UberGraphFrame",
			}), ClassDefaultObject);
		}
	}
	
	return OnAssetCreation(Blueprint);
}

void IBlueprintGeneratedClassImporter::CreateVariables(UBlueprint* BP, FString OuterName, const TArray<TSharedPtr<FJsonValue>> ChildrensObjectPath, UEdGraph* FunctionGraph) {
	for (const TSharedPtr<FJsonValue>& ChildrenObjectPath : ChildrensObjectPath) {
		FUObjectExport ChildrenExport = AssetContainer.GetExportByObjectPath(ChildrenObjectPath->AsObject());

		const FString ChildrenOuterName = ChildrenExport.GetOuter().ToString();
		if (!ChildrenOuterName.Equals(OuterName)) {
			continue;
		}

		const FString ChildrenName = ChildrenExport.GetName().ToString();
		const FString ChildrenPropertyFlags = ChildrenExport.JsonObject->GetStringField(TEXT("PropertyFlags"));

		FName NewVarName(*ChildrenName);
		UE_LOG(LogTemp, Log, TEXT("Variable name : '%s'."), *ChildrenName);

		// See EdGraphSchema_K2.cpp for the types
		FEdGraphPinType PinType = GetPinType(ChildrenExport);
		if (PinType.PinCategory.IsEmpty() || !PinType.PinSubCategoryObject.IsValid()) {
			UE_LOG(LogTemp, Warning, TEXT("Skipping %s: invalid type"), *ChildrenName);
			continue;
		}

		if (FunctionGraph == nullptr) {
			if (!FBlueprintEditorUtils::AddMemberVariable(BP, NewVarName, PinType)) {
				UE_LOG(LogTemp, Warning, TEXT("Variable '%s' already exists"), *ChildrenName);
				continue;
			}
		}
		else {
			// It's a variable inside the function
			if (ChildrenPropertyFlags.Contains(TEXT("Edit"))) {
				if (!FBlueprintEditorUtils::AddLocalVariable(BP, FunctionGraph, NewVarName, PinType)) {
					UE_LOG(LogTemp, Warning, TEXT("Local variable '%s' already exists in function '%s'."), *ChildrenName, *FunctionGraph->GetName());
				}
				continue;
			}

			/// In/Output nodes
			UK2Node_FunctionEntry* EntryNode = nullptr;
			for (UEdGraphNode* Node : FunctionGraph->Nodes)
			{
				EntryNode = Cast<UK2Node_FunctionEntry>(Node);
				if (EntryNode) {
					break;
				}
			}

			if (ChildrenPropertyFlags.Contains(TEXT("InParm"))) {
				EntryNode->CreateUserDefinedPin(ChildrenName, PinType, EGPD_Input);
			}

			// Create the output node
			if (ChildrenPropertyFlags.Contains(TEXT("OutParm"))) {
				UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
				if (!ResultNode) {
					UE_LOG(LogTemp, Error, TEXT("Failed to find or create FunctionResult node for '%s'."), *ChildrenName);
					continue;
				}

				ResultNode->CreateUserDefinedPin(ChildrenName, PinType, EGPD_Output);
			}
		}

		// Put the property flags of the variables
		FBPVariableDescription* VarDesc = nullptr;
		for (FBPVariableDescription& V : BP->NewVariables)
		{
			if (V.VarName == NewVarName)
			{
				VarDesc = &V;
				break;
			}
		}

		if (!VarDesc) {
			UE_LOG(LogTemp, Warning, TEXT("Could not find newly added variable '%s' in NewVariables."), *ChildrenName);
			continue;
		}

		if (!ChildrenPropertyFlags.IsEmpty()) {
			if (ChildrenPropertyFlags.Contains(TEXT("Edit"))) {
				VarDesc->PropertyFlags |= CPF_Edit;
			}
			if (ChildrenPropertyFlags.Contains(TEXT("BlueprintVisible"))) {
				VarDesc->PropertyFlags |= CPF_BlueprintVisible;
			}
			if (ChildrenPropertyFlags.Contains(TEXT("DisableEditOnInstance"))) {
				VarDesc->PropertyFlags |= CPF_DisableEditOnInstance;
			}
		}
		///
	}
}

FEdGraphPinType IBlueprintGeneratedClassImporter::GetPinType(FUObjectExport Export)
{
	FEdGraphPinType PinType;

	const FString Type = Export.GetType().ToString();

	if (Type == TEXT("ArrayProperty"))
	{
		// Arrays have an Inner property that describes element type
		FUObjectExport InnerExport = AssetContainer.GetExportByObjectPath(Export.JsonObject->GetObjectField(TEXT("SimpleConstructionScript")));

		if (InnerExport.IsValid()) {
			PinType = GetPinType(InnerExport);
			PinType.ContainerType = EPinContainerType::Array;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ArrayProperty missing Inner export"));
		}

		return PinType;
	}

	if (Type == TEXT("StrProperty"))
	{
		PinType.PinCategory = TEXT("string");
	}
	else if (Type == TEXT("IntProperty"))
	{
		PinType.PinCategory = TEXT("int");
	}
	else if (Type == TEXT("FloatProperty"))
	{
		PinType.PinCategory = TEXT("float");
	}
	else if (Type == TEXT("BoolProperty"))
	{
		PinType.PinCategory = TEXT("bool");
	}
	else if (Type == TEXT("StructProperty"))
	{
		PinType.PinCategory = TEXT("struct");
		UObject* StructObject = LoadStruct(Export.JsonObject->GetObjectField(TEXT("Struct")));
		if (StructObject)
			PinType.PinSubCategoryObject = Cast<UScriptStruct>(StructObject);
	}
	else if (Type == TEXT("ObjectProperty"))
	{
		PinType.PinCategory = TEXT("object");
		const TSharedPtr<FJsonObject> PropertyClassObject = Export.JsonObject->GetObjectField(TEXT("PropertyClass"));
		TObjectPtr<UObject> Object;
		LoadExport(&PropertyClassObject, Object);
		if (Object) {
			PinType.PinSubCategoryObject = Object.Get();
		}

	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Unknown variable type: %s"), *Type);
	}

	return PinType;
}

void IBlueprintGeneratedClassImporter::ReadFuncMap(UBlueprint* BP) {
	// Get the ubergraph
	UEdGraph* UberGraph = GetUberGraph(BP);

	// Remove all the event nodes before creating the new ones
	RemoveEventNodes(UberGraph);

	const TSharedPtr<FJsonObject>* FuncMapObject;
	if (GetAssetData()->TryGetObjectField(TEXT("FuncMap"), FuncMapObject)) {
		for (const auto& Pair : (*FuncMapObject)->Values) {
			FUObjectExport FunctionExport = AssetContainer.GetExportByObjectPath(Pair.Value->AsObject());

			const FString FunctionName = FunctionExport.GetName().ToString();

			UEdGraph* ExistingGraph = FindObject<UEdGraph>(BP, *FunctionName);
			if (ExistingGraph) {
				continue;
			}

			const FString FunctionFlags = FunctionExport.JsonObject->GetStringField(TEXT("FunctionFlags"));

			// Create function
			if (!FunctionFlags.Contains("FUNC_Event")) {
				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
					BP,
					*FunctionName,
					UEdGraph::StaticClass(),
					UEdGraphSchema_K2::StaticClass()
				);

				FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, NewGraph, true, nullptr);

				UEdGraphPin* EntryPin = nullptr;
				for (UEdGraphNode* Node : NewGraph->Nodes)
				{
					if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
					{
						EntryNode->CustomGeneratedFunctionName = FName(*FunctionName);
						break;
					}
				}

				CreateVariables(BP, FunctionName, FunctionExport.JsonObject->GetArrayField(TEXT("Children")), NewGraph);
			}
			// Create event
			else {
				const TSharedPtr<FJsonObject> SuperStruct = FunctionExport.JsonObject->GetObjectField(TEXT("SuperStruct"));
				const FString ObjectName = SuperStruct->GetStringField(TEXT("ObjectName")).Replace(TEXT("Function'"), TEXT("")).Replace(TEXT("'"), TEXT(""));
				const FString ObjectPath = SuperStruct->GetStringField(TEXT("ObjectPath"));
				FString OuterName, EventName;
				ObjectName.Split(TEXT(":"), &OuterName, &EventName);

				UFunction* Function = BP->ParentClass->FindFunctionByName(FName(*EventName));
				if (Function && UberGraph) {
					UK2Node_Event* EventNode = NewObject<UK2Node_Event>(UberGraph);
					EventNode->EventReference.SetFromField<UFunction>(Function, false);
					EventNode->bOverrideFunction = false;
					EventNode->CreateNewGuid();
					EventNode->PostPlacedNewNode();
					EventNode->AllocateDefaultPins();
					UberGraph->AddNode(EventNode, true, false);
				}
			}
		}
	}
}

void IBlueprintGeneratedClassImporter::HandleSimpleConstructionScript(UBlueprint* BP, USCS_Node* Node, const TArray<TSharedPtr<FJsonValue>> NodesObject, bool bIsRoot) {
	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;

	for (const TSharedPtr<FJsonValue>& NodeObject : NodesObject) {
		FUObjectExport SCSNodeExport = AssetContainer.GetExportByObjectPath(NodeObject->AsObject());
		UClass* ComponentClass = LoadClass(SCSNodeExport.GetProperties()->GetObjectField(TEXT("ComponentClass")));
		USCS_Node* SCSNode = SCS->CreateNode(ComponentClass, *SCSNodeExport.GetProperties()->GetStringField(TEXT("InternalVariableName")));

		FUObjectExport ComponentTemplateExport = AssetContainer.GetExportByObjectPath(SCSNodeExport.GetProperties()->GetObjectField(TEXT("ComponentTemplate")));
		ReadComponentTemplate(BP, SCSNode->ComponentTemplate, ComponentTemplateExport);

		GetObjectSerializer()->DeserializeObjectProperties(KeepPropertiesShared(SCSNodeExport.GetProperties(), {
			"ParentComponentOrVariableName",
			"bIsParentComponentNative",
			"VariableGuid"
		}), SCSNode);

		if (bIsRoot) {
			SCS->AddNode(SCSNode);
		}
		else {
			Node->AddChildNode(SCSNode);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	
		if (SCSNodeExport.GetProperties()->HasField(TEXT("ChildNodes"))) {
			const TArray<TSharedPtr<FJsonValue>> ChildNodesObject = SCSNodeExport.GetProperties()->GetArrayField("ChildNodes");
			HandleSimpleConstructionScript(BP, SCSNode, ChildNodesObject, false);
		}
	}
}

void IBlueprintGeneratedClassImporter::HandleInheritableComponentHandler(UBlueprint* BP, FUObjectExport InheritableComponentHandlerExport) {
	TArray<USCS_Node*> AllNodes;
	GetAllSCSNodes(BP, AllNodes);

	const TArray<TSharedPtr<FJsonValue>> RecordsData = InheritableComponentHandlerExport.GetProperties()->GetArrayField(TEXT("Records"));
	for (const TSharedPtr<FJsonValue>& RecordData : RecordsData) {
		const TSharedPtr<FJsonObject> RecordDataObject = RecordData->AsObject();
		const TSharedPtr<FJsonObject> ComponentClassData = RecordDataObject->GetObjectField(TEXT("ComponentClass"));
		const TSharedPtr<FJsonObject> ComponentKeyData = RecordDataObject->GetObjectField(TEXT("ComponentKey"));

		USCS_Node* SCSNode = FindSCSNodeByName(AllNodes, *ComponentKeyData->GetStringField(TEXT("SCSVariableName")));
		if (!SCSNode) {
			UE_LOG(LogTemp, Warning, TEXT("SCS Node of name '%s' not found"), *ComponentKeyData->GetStringField(TEXT("SCSVariableName")));
			continue;
		}

		UActorComponent* ComponentTemplate = SCSNode->ComponentTemplate;

		FUObjectExport ComponentTemplateExport = AssetContainer.GetExportByObjectPath(RecordDataObject->GetObjectField(TEXT("ComponentTemplate")));
		ReadComponentTemplate(BP, ComponentTemplate, ComponentTemplateExport);
	}
}

void IBlueprintGeneratedClassImporter::ReadComponentTemplate(UBlueprint* BP, UActorComponent* ComponentTemplate, FUObjectExport ComponentTemplateExport) {
	ComponentTemplate->Rename(*ComponentTemplateExport.GetName().ToString(), nullptr, REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
	GetObjectSerializer()->DeserializeObjectProperties(ComponentTemplateExport.GetProperties(), ComponentTemplate);
}