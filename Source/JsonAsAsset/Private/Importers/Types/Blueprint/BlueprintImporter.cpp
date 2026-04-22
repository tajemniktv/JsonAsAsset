/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintImporter.h"

#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Timeline.h"
#include "Modules/Log.h"
#include "KismetCompilerModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/EngineUtilities.h"
#include "Engine/Blueprint.h"
#include "Utilities/BlueprintUtilities.h"
#include "Utilities/JsonUtilities.h"

namespace
{
const FJBlueprintImportSettings& GetBlueprintImportSettings()
{
	return GetSettings()->BlueprintImport;
}

bool IsStrictBlueprintImport()
{
	return GetBlueprintImportSettings().StrictMode;
}

bool IsVerboseBlueprintImport()
{
	return GetBlueprintImportSettings().LogDetail == EJBlueprintLogDetail::Verbose;
}

bool ShouldCompileImmediatelyFromSettings()
{
	return GetBlueprintImportSettings().CompilePolicy == EJBlueprintCompilePolicy::Immediate;
}

bool UseBlueprintCompatibilityFallback()
{
	const UJsonAsAssetSettings* Settings = GetSettings();
	return Settings && Settings->CompatibilityFallback.BlueprintGeneratedClass;
}

bool IsBlueprintObjectValid(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr && Blueprint->GeneratedClass != nullptr;
}

void LogBlueprintImportWarning(const FString& Message, const bool bCritical)
{
	if (bCritical && IsStrictBlueprintImport()) {
		UE_LOG(LogJsonAsAsset, Error, TEXT("%s"), *Message);
		return;
	}

	UE_LOG(LogJsonAsAsset, Warning, TEXT("%s"), *Message);
}

void RemoveGraphNodes(UEdGraph* Graph)
{
	if (!Graph) {
		return;
	}

	for (UEdGraphNode* Node : Graph->Nodes) {
		if (!Node) {
			continue;
		}

		Node->BreakAllNodeLinks();
		Node->ConditionalBeginDestroy();
	}

	Graph->Nodes.Empty();
}

void ClearExistingEventNodes(UBlueprint* Blueprint)
{
	if (!Blueprint) {
		return;
	}

	for (UEdGraph* Graph : Blueprint->UbergraphPages) {
		if (!Graph) {
			continue;
		}

		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (Cast<UK2Node_Event>(Node) || Cast<UK2Node_CustomEvent>(Node)) {
				NodesToRemove.Add(Node);
			}
		}

		for (UEdGraphNode* NodeToRemove : NodesToRemove) {
			if (NodeToRemove) {
				NodeToRemove->BreakAllNodeLinks();
				Graph->RemoveNode(NodeToRemove);
				NodeToRemove->ConditionalBeginDestroy();
			}
		}
	}
}

void ParseFlags(const FString& FlagsString, TArray<FString>& OutFlags)
{
	OutFlags.Empty();
	FlagsString.ParseIntoArray(OutFlags, TEXT(" | "), true);
}

bool HasFlag(const TArray<FString>& Flags, const FString& Flag)
{
	return Flags.ContainsByPredicate([&Flag](const FString& Existing) {
		return Existing.Equals(Flag, ESearchCase::CaseSensitive);
	});
}

void GetAllSCSNodes(UBlueprint* InBlueprint, TArray<USCS_Node*>& OutNodes)
{
	if (!InBlueprint) {
		return;
	}

	if (InBlueprint->SimpleConstructionScript) {
		OutNodes.Append(InBlueprint->SimpleConstructionScript->GetAllNodes());
	}

	for (UBlueprint* ParentBlueprint = Cast<UBlueprint>(InBlueprint->ParentClass ? InBlueprint->ParentClass->ClassGeneratedBy : nullptr);
		ParentBlueprint;
		ParentBlueprint = Cast<UBlueprint>(ParentBlueprint->ParentClass ? ParentBlueprint->ParentClass->ClassGeneratedBy : nullptr)) {
		if (ParentBlueprint->SimpleConstructionScript) {
			OutNodes.Append(ParentBlueprint->SimpleConstructionScript->GetAllNodes());
		}
	}
}

USCS_Node* FindSCSNodeByVariableName(const TArray<USCS_Node*>& Nodes, const FName VariableName)
{
	for (USCS_Node* Node : Nodes) {
		if (!Node) {
			continue;
		}

		if (Node->GetVariableName() == VariableName) {
			return Node;
		}
	}

	return nullptr;
}

UBlueprint* FindExistingBlueprint(UPackage* Package, const FString& AssetName)
{
	if (!Package) {
		return nullptr;
	}

	if (UBlueprint* Existing = FindObject<UBlueprint>(Package, *AssetName)) {
		return Existing;
	}

	const FString ExistingObjectPath = Package->GetPathName() + TEXT(".") + AssetName;
	return LoadObject<UBlueprint>(nullptr, *ExistingObjectPath);
}

void MoveBlueprintToTransient(UBlueprint* Blueprint, const FString& Reason)
{
	if (!Blueprint) {
		return;
	}

	UE_LOG(LogJsonAsAsset, Warning, TEXT("Moving existing Blueprint '%s' to transient package: %s"),
		*Blueprint->GetPathName(), *Reason);
	MoveToTransientPackageAndRename(Blueprint);
}

UEdGraph* GetOrCreateUberGraph(UBlueprint* Blueprint)
{
	if (!Blueprint) {
		return nullptr;
	}

	if (Blueprint->UbergraphPages.Num() > 0 && Blueprint->UbergraphPages[0]) {
		return Blueprint->UbergraphPages[0];
	}

	UEdGraph* UberGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		TEXT("EventGraph"),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!UberGraph) {
		return nullptr;
	}

	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, UberGraph);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	Schema->CreateDefaultNodesForGraph(*UberGraph);
	return UberGraph;
}

UEdGraph* FindFunctionGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint) {
		return nullptr;
	}

	const FName TargetName(*GraphName);

	for (UEdGraph* Graph : Blueprint->FunctionGraphs) {
		if (Graph && Graph->GetFName() == TargetName) {
			return Graph;
		}
	}

	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) {
		if (Graph && Graph->GetFName() == TargetName) {
			return Graph;
		}
	}

	return nullptr;
}

FName MakeSafeGraphName(UBlueprint* Blueprint, const FString& DesiredName, const TCHAR* Suffix)
{
	if (!Blueprint) {
		return FName(*DesiredName);
	}

	UObject* ExistingObject = FindObject<UObject>(Blueprint, *DesiredName);
	if (ExistingObject && !ExistingObject->IsA<UEdGraph>()) {
		const FString BaseName = DesiredName + Suffix;
		const FName UniqueName = MakeUniqueObjectName(Blueprint, UEdGraph::StaticClass(), FName(*BaseName));
		UE_LOG(LogJsonAsAsset, Warning, TEXT("Blueprint graph name collision on '%s' with '%s'; using '%s'"),
			*DesiredName, *ExistingObject->GetClass()->GetName(), *UniqueName.ToString());
		return UniqueName;
	}

	return FName(*DesiredName);
}

bool ResolvePinType(IBlueprintImporter* Importer, const TSharedPtr<FJsonObject>& PropertyObject, FEdGraphPinType& OutPinType)
{
	OutPinType = FEdGraphPinType();
	if (!PropertyObject.IsValid() || !PropertyObject->HasField(TEXT("Type"))) {
		return false;
	}

	const FString Type = PropertyObject->GetStringField(TEXT("Type"));
	TArray<FString> PropertyFlags;
	if (PropertyObject->HasField(TEXT("PropertyFlags"))) {
		ParseFlags(PropertyObject->GetStringField(TEXT("PropertyFlags")), PropertyFlags);
	}
	OutPinType.bIsConst = HasFlag(PropertyFlags, TEXT("ConstParm"));
	OutPinType.bIsReference = HasFlag(PropertyFlags, TEXT("ReferenceParm"));
	OutPinType.bIsUObjectWrapper = HasFlag(PropertyFlags, TEXT("UObjectWrapper"));

	const auto LoadSubCategoryObject = [&](const TCHAR* FieldName, UObject*& OutObject) -> bool {
		OutObject = nullptr;
		if (!PropertyObject->HasField(FieldName)) {
			return false;
		}

		TObjectPtr<UObject> LoadedObject;
		TSharedPtr<FJsonObject> ObjectPath = PropertyObject->GetObjectField(FieldName);
		Importer->LoadExport(&ObjectPath, LoadedObject);
		OutObject = LoadedObject.Get();
		return OutObject != nullptr;
	};

	if (Type == TEXT("ArrayProperty") && PropertyObject->HasField(TEXT("Inner"))) {
		FEdGraphPinType InnerPinType;
		if (!ResolvePinType(Importer, PropertyObject->GetObjectField(TEXT("Inner")), InnerPinType)) {
			return false;
		}

		OutPinType = InnerPinType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}

	if (Type == TEXT("SetProperty") && PropertyObject->HasField(TEXT("ElementProp"))) {
		FEdGraphPinType InnerPinType;
		if (!ResolvePinType(Importer, PropertyObject->GetObjectField(TEXT("ElementProp")), InnerPinType)) {
			return false;
		}

		OutPinType = InnerPinType;
		OutPinType.ContainerType = EPinContainerType::Set;
		return true;
	}

	if (Type == TEXT("MapProperty") && PropertyObject->HasField(TEXT("KeyProp")) && PropertyObject->HasField(TEXT("ValueProp"))) {
		FEdGraphPinType KeyPinType;
		FEdGraphPinType ValuePinType;
		if (!ResolvePinType(Importer, PropertyObject->GetObjectField(TEXT("KeyProp")), KeyPinType) ||
			!ResolvePinType(Importer, PropertyObject->GetObjectField(TEXT("ValueProp")), ValuePinType)) {
			return false;
		}

		OutPinType = KeyPinType;
		OutPinType.ContainerType = EPinContainerType::Map;
		OutPinType.PinValueType = FEdGraphTerminalType::FromPinType(ValuePinType);
		return true;
	}

	if (Type == TEXT("BoolProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return true;
	}
	if (Type == TEXT("ByteProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		UObject* EnumObject = nullptr;
		if (LoadSubCategoryObject(TEXT("Enum"), EnumObject)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
			OutPinType.PinSubCategoryObject = EnumObject;
		}
		return true;
	}
	if (Type == TEXT("IntProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return true;
	}
	if (Type == TEXT("Int64Property")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return true;
	}
	if (Type == TEXT("FloatProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (Type == TEXT("DoubleProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (Type == TEXT("NameProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return true;
	}
	if (Type == TEXT("StrProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (Type == TEXT("TextProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return true;
	}
	if (Type == TEXT("StructProperty")) {
		UObject* StructObject = nullptr;
		if (LoadSubCategoryObject(TEXT("Struct"), StructObject)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = StructObject;
			return true;
		}

		return false;
	}
	if (Type == TEXT("EnumProperty")) {
		UObject* EnumObject = nullptr;
		if (LoadSubCategoryObject(TEXT("Enum"), EnumObject)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
			OutPinType.PinSubCategoryObject = EnumObject;
			return true;
		}

		return false;
	}
	if (Type == TEXT("InterfaceProperty")) {
		UObject* InterfaceClass = nullptr;
		if (LoadSubCategoryObject(TEXT("InterfaceClass"), InterfaceClass)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
			OutPinType.PinSubCategoryObject = InterfaceClass;
			return true;
		}

		return false;
	}
	if (Type == TEXT("ObjectProperty") || Type == TEXT("ObjectPtrProperty")) {
		UObject* ObjectClass = nullptr;
		if (LoadSubCategoryObject(TEXT("PropertyClass"), ObjectClass)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = ObjectClass;
			return true;
		}

		return false;
	}
	if (Type == TEXT("SoftObjectProperty")) {
		UObject* ObjectClass = nullptr;
		if (LoadSubCategoryObject(TEXT("PropertyClass"), ObjectClass)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
			OutPinType.PinSubCategoryObject = ObjectClass;
			return true;
		}

		return false;
	}
	if (Type == TEXT("ClassProperty")) {
		UObject* MetaClass = nullptr;
		if (LoadSubCategoryObject(TEXT("MetaClass"), MetaClass)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
			OutPinType.PinSubCategoryObject = MetaClass;
			return true;
		}

		return false;
	}
	if (Type == TEXT("SoftClassProperty")) {
		UObject* MetaClass = nullptr;
		if (LoadSubCategoryObject(TEXT("MetaClass"), MetaClass)) {
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
			OutPinType.PinSubCategoryObject = MetaClass;
			return true;
		}

		return false;
	}
	if (Type == TEXT("FieldPathProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_FieldPath;
		return true;
	}
	if (Type == TEXT("DelegateProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Delegate;
		return true;
	}
	if (Type == TEXT("MulticastDelegateProperty") || Type == TEXT("MulticastInlineDelegateProperty")) {
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		return true;
	}

	return false;
}

EPropertyFlags ResolvePropertyFlags(const TArray<FString>& Flags)
{
	static const TMap<FString, EPropertyFlags> PropertyFlagsByString{
		{TEXT("None"), CPF_None},
		{TEXT("Transient"), CPF_Transient},
		{TEXT("DuplicateTransient"), CPF_DuplicateTransient},
		{TEXT("TextExportTransient"), CPF_TextExportTransient},
		{TEXT("TextExportTrnasient"), CPF_TextExportTransient},
		{TEXT("NonPIEDuplicateTransient"), CPF_NonPIEDuplicateTransient},
		{TEXT("BlueprintVisible"), CPF_BlueprintVisible},
		{TEXT("BlueprintReadOnly"), CPF_BlueprintReadOnly},
		{TEXT("BlueprintAssignable"), CPF_BlueprintAssignable},
		{TEXT("BlueprintCallable"), CPF_BlueprintCallable},
		{TEXT("BlueprintAuthorityOnly"), CPF_BlueprintAuthorityOnly},
		{TEXT("Edit"), CPF_Edit},
		{TEXT("EditFixedSize"), CPF_EditFixedSize},
		{TEXT("EditConst"), CPF_EditConst},
		{TEXT("DisableEditOnInstance"), CPF_DisableEditOnInstance},
		{TEXT("DisableEditOnTemplate"), CPF_DisableEditOnTemplate},
		{TEXT("Parm"), CPF_Parm},
		{TEXT("ConstParm"), CPF_ConstParm},
		{TEXT("ReferenceParm"), CPF_ReferenceParm},
		{TEXT("OutParm"), CPF_OutParm},
		{TEXT("ReturnParm"), CPF_ReturnParm},
		{TEXT("Net"), CPF_Net},
		{TEXT("RepNotify"), CPF_RepNotify},
		{TEXT("RepSkip"), CPF_RepSkip},
		{TEXT("Interp"), CPF_Interp},
		{TEXT("SaveGame"), CPF_SaveGame},
		{TEXT("Config"), CPF_Config},
		{TEXT("GlobalConfig"), CPF_GlobalConfig},
		{TEXT("NoClear"), CPF_NoClear},
		{TEXT("ExposeOnSpawn"), CPF_ExposeOnSpawn},
		{TEXT("ContainsInstancedReference"), CPF_ContainsInstancedReference},
		{TEXT("InstancedReference"), CPF_InstancedReference},
		{TEXT("AssetRegistrySearchable"), CPF_AssetRegistrySearchable},
		{TEXT("SimpleDisplay"), CPF_SimpleDisplay},
		{TEXT("AdvancedDisplay"), CPF_AdvancedDisplay},
		{TEXT("Protected"), CPF_Protected},
		{TEXT("UObjectWrapper"), CPF_UObjectWrapper},
		{TEXT("PersistentInstance"), CPF_PersistentInstance},
		{TEXT("HasGetValueTypeHash"), CPF_HasGetValueTypeHash},
		{TEXT("SkipSerialization"), CPF_SkipSerialization},
		{TEXT("EditorOnly"), CPF_EditorOnly},
		{TEXT("NonTransactional"), CPF_NonTransactional},
		{TEXT("AutoWeak"), CPF_AutoWeak},
		{TEXT("Deprecated"), CPF_Deprecated}
	};

	EPropertyFlags Result = CPF_None;
	for (const FString& FlagString : Flags) {
		if (const EPropertyFlags* Resolved = PropertyFlagsByString.Find(FlagString)) {
			Result |= *Resolved;
		}
	}

	return Result;
}

EFunctionFlags ResolveFunctionFlags(const TArray<FString>& Flags)
{
	static const TMap<FString, EFunctionFlags> FunctionFlagsByString{
		{TEXT("FUNC_None"), FUNC_None},
		{TEXT("FUNC_Final"), FUNC_Final},
		{TEXT("FUNC_RequiredAPI"), FUNC_RequiredAPI},
		{TEXT("FUNC_BlueprintAuthorityOnly"), FUNC_BlueprintAuthorityOnly},
		{TEXT("FUNC_BlueprintCosmetic"), FUNC_BlueprintCosmetic},
		{TEXT("FUNC_Net"), FUNC_Net},
		{TEXT("FUNC_NetReliable"), FUNC_NetReliable},
		{TEXT("FUNC_NetRequest"), FUNC_NetRequest},
		{TEXT("FUNC_Exec"), FUNC_Exec},
		{TEXT("FUNC_Native"), FUNC_Native},
		{TEXT("FUNC_Event"), FUNC_Event},
		{TEXT("FUNC_NetResponse"), FUNC_NetResponse},
		{TEXT("FUNC_Static"), FUNC_Static},
		{TEXT("FUNC_NetMulticast"), FUNC_NetMulticast},
		{TEXT("FUNC_UbergraphFunction"), FUNC_UbergraphFunction},
		{TEXT("FUNC_MulticastDelegate"), FUNC_MulticastDelegate},
		{TEXT("FUNC_Public"), FUNC_Public},
		{TEXT("FUNC_Private"), FUNC_Private},
		{TEXT("FUNC_Protected"), FUNC_Protected},
		{TEXT("FUNC_Delegate"), FUNC_Delegate},
		{TEXT("FUNC_NetServer"), FUNC_NetServer},
		{TEXT("FUNC_HasOutParms"), FUNC_HasOutParms},
		{TEXT("FUNC_HasDefaults"), FUNC_HasDefaults},
		{TEXT("FUNC_NetClient"), FUNC_NetClient},
		{TEXT("FUNC_DLLImport"), FUNC_DLLImport},
		{TEXT("FUNC_BlueprintCallable"), FUNC_BlueprintCallable},
		{TEXT("FUNC_BlueprintEvent"), FUNC_BlueprintEvent},
		{TEXT("FUNC_BlueprintPure"), FUNC_BlueprintPure},
		{TEXT("FUNC_EditorOnly"), FUNC_EditorOnly},
		{TEXT("FUNC_Const"), FUNC_Const},
		{TEXT("FUNC_NetValidate"), FUNC_NetValidate}
	};

	EFunctionFlags Result = FUNC_None;
	for (const FString& FlagString : Flags) {
		if (const EFunctionFlags* Resolved = FunctionFlagsByString.Find(FlagString)) {
			Result |= *Resolved;
		}
	}

	return Result;
}

void CreateBlueprintMemberVariables(IBlueprintImporter* Importer, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& AssetData)
{
	if (!Importer || !Blueprint || !AssetData.IsValid() || !AssetData->HasField(TEXT("ChildProperties"))) {
		return;
	}

	/* Widget blueprints maintain widget-generated variables and can crash when forced through generic member-variable creation. */
	if (Importer->GetAssetType().Contains(TEXT("WidgetBlueprintGeneratedClass"))) {
		if (IsVerboseBlueprintImport()) {
			UE_LOG(LogJsonAsAsset, Warning, TEXT("Skipping generic member variable reconstruction for Widget Blueprint '%s'"), *Importer->GetAssetName());
		}
		return;
	}

	for (const TSharedPtr<FJsonValue>& PropertyValue : AssetData->GetArrayField(TEXT("ChildProperties"))) {
		const TSharedPtr<FJsonObject> PropertyObject = PropertyValue->AsObject();
		if (!PropertyObject.IsValid() || !PropertyObject->HasField(TEXT("Name")) || !PropertyObject->HasField(TEXT("PropertyFlags"))) {
			continue;
		}

		const FString VariableNameString = PropertyObject->GetStringField(TEXT("Name"));
		if (VariableNameString.IsEmpty()) {
			continue;
		}

		TArray<FString> PropertyFlags;
		ParseFlags(PropertyObject->GetStringField(TEXT("PropertyFlags")), PropertyFlags);

		/* Only member variables here; function params are handled while constructing function graphs. */
		if (HasFlag(PropertyFlags, TEXT("Parm"))) {
			continue;
		}

		FEdGraphPinType PinType;
		if (!ResolvePinType(Importer, PropertyObject, PinType)) {
			if (IsVerboseBlueprintImport()) {
				UE_LOG(LogJsonAsAsset, Warning, TEXT("Skipped unresolved Blueprint variable '%s'"), *VariableNameString);
			}
			continue;
		}

		const FName VariableName(*VariableNameString);
		bool bVariableExists = false;
		for (const FBPVariableDescription& ExistingVariable : Blueprint->NewVariables) {
			if (ExistingVariable.VarName == VariableName) {
				bVariableExists = true;
				break;
			}
		}
		if (!bVariableExists) {
			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType)) {
				continue;
			}
		}

		uint64* BlueprintVariableFlags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, VariableName);
		if (BlueprintVariableFlags) {
			*BlueprintVariableFlags |= static_cast<uint64>(ResolvePropertyFlags(PropertyFlags));
		}

		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VariableName, HasFlag(PropertyFlags, TEXT("DisableEditOnInstance")));
		FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VariableName, HasFlag(PropertyFlags, TEXT("BlueprintReadOnly")));
		FBlueprintEditorUtils::SetInterpFlag(Blueprint, VariableName, HasFlag(PropertyFlags, TEXT("Interp")));
		FBlueprintEditorUtils::SetVariableTransientFlag(Blueprint, VariableName, HasFlag(PropertyFlags, TEXT("Transient")));
		FBlueprintEditorUtils::SetVariableSaveGameFlag(Blueprint, VariableName, HasFlag(PropertyFlags, TEXT("SaveGame")));
		FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(Blueprint, VariableName, HasFlag(PropertyFlags, TEXT("AdvancedDisplay")));
		FBlueprintEditorUtils::SetVariableDeprecatedFlag(Blueprint, VariableName, HasFlag(PropertyFlags, TEXT("Deprecated")));

		if (HasFlag(PropertyFlags, TEXT("ExposeOnSpawn"))) {
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VariableName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("True"));
		}
	}
}

void SetupDynamicBindings(IBlueprintImporter* Importer, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& AssetData)
{
	if (!Importer || !Blueprint || !AssetData.IsValid() || !AssetData->HasTypedField<EJson::Array>(TEXT("DynamicBindingObjects"))) {
		return;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!EventGraph) {
		return;
	}

	static const TArray<FString> BindingArrayFieldNames{
		TEXT("ComponentDelegateBindings"),
		TEXT("WidgetAnimationDelegateBindings"),
		TEXT("InputActionDelegateBindings"),
		TEXT("InputAxisDelegateBindings"),
		TEXT("InputAxisKeyDelegateBindings"),
		TEXT("InputKeyDelegateBindings"),
		TEXT("InputTouchDelegateBindings"),
		TEXT("InputActionValueBindings"),
		TEXT("InputDebugKeyDelegateBindings")
	};

	for (const TSharedPtr<FJsonValue>& BindingPathValue : AssetData->GetArrayField(TEXT("DynamicBindingObjects"))) {
		const TSharedPtr<FJsonObject> BindingPathObject = BindingPathValue->AsObject();
		if (!BindingPathObject.IsValid()) {
			continue;
		}

		FUObjectExport* BindingExport = Importer->AssetContainer->GetExportByObjectPath(BindingPathObject);
		if (!BindingExport || !BindingExport->IsJsonValid() || !BindingExport->Has(TEXT("Properties"))) {
			continue;
		}

		const TSharedPtr<FJsonObject> BindingProps = BindingExport->GetProperties();
		if (!BindingProps.IsValid()) {
			continue;
		}

		for (const FString& BindingArrayFieldName : BindingArrayFieldNames) {
			if (!BindingProps->HasTypedField<EJson::Array>(*BindingArrayFieldName)) {
				continue;
			}

			for (const TSharedPtr<FJsonValue>& DelegateBindingValue : BindingProps->GetArrayField(*BindingArrayFieldName)) {
				const TSharedPtr<FJsonObject> DelegateBinding = DelegateBindingValue->AsObject();
				if (!DelegateBinding.IsValid()) {
					continue;
				}

				FString FunctionName;
				if (!DelegateBinding->TryGetStringField(TEXT("FunctionNameToBind"), FunctionName) || FunctionName.IsEmpty()) {
					continue;
				}

				if (BindingArrayFieldName != TEXT("ComponentDelegateBindings")) {
					if (IsVerboseBlueprintImport()) {
						UE_LOG(LogJsonAsAsset, Log, TEXT("Captured dynamic binding '%s' -> '%s'"),
							*BindingArrayFieldName, *FunctionName);
					}
					continue;
				}

				FString DelegatePropertyName;
				DelegateBinding->TryGetStringField(TEXT("DelegatePropertyName"), DelegatePropertyName);

				FString ComponentName;
				DelegateBinding->TryGetStringField(TEXT("ComponentPropertyName"), ComponentName);

				FGraphNodeCreator<UK2Node_ComponentBoundEvent> NodeCreator(*EventGraph);
				UK2Node_ComponentBoundEvent* EventNode = NodeCreator.CreateNode();
				EventNode->ComponentPropertyName = *ComponentName;
				EventNode->DelegatePropertyName = *DelegatePropertyName;
				EventNode->CustomFunctionName = *FunctionName;
				EventNode->AllocateDefaultPins();
				NodeCreator.Finalize();
			}
		}
	}
}

void AddFunctionPinsFromChildProperties(IBlueprintImporter* Importer, UBlueprint* Blueprint, UEdGraph* Graph, const FUObjectExport* FunctionExport)
{
	if (!Importer || !Blueprint || !Graph || !FunctionExport || !FunctionExport->Has(TEXT("ChildProperties"))) {
		return;
	}

	UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(Graph));
	UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
	if (!EntryNode || !ResultNode) {
		return;
	}

	for (const FUObjectJsonValueExport& ChildValue : FunctionExport->GetArray(TEXT("ChildProperties"))) {
		const TSharedPtr<FJsonObject> ChildObject = ChildValue.JsonObject;
		if (!ChildObject.IsValid() || !ChildObject->HasField(TEXT("Name"))) {
			continue;
		}

		if (!ChildObject->HasField(TEXT("PropertyFlags"))) {
			continue;
		}

		TArray<FString> PropertyFlags;
		ParseFlags(ChildObject->GetStringField(TEXT("PropertyFlags")), PropertyFlags);
		if (!HasFlag(PropertyFlags, TEXT("Parm"))) {
			continue;
		}

		FEdGraphPinType PinType;
		if (!ResolvePinType(Importer, ChildObject, PinType)) {
			if (IsVerboseBlueprintImport()) {
				UE_LOG(LogJsonAsAsset, Warning, TEXT("Skipped unresolved function pin '%s' in '%s'"),
					*ChildObject->GetStringField(TEXT("Name")), *FunctionExport->GetName().ToString());
			}
			continue;
		}

		const FName PinName(*ChildObject->GetStringField(TEXT("Name")));
		if (HasFlag(PropertyFlags, TEXT("OutParm"))) {
			ResultNode->CreateUserDefinedPin(PinName, PinType, EGPD_Output);
		} else {
			EntryNode->CreateUserDefinedPin(PinName, PinType, EGPD_Input);
		}
	}
}

void AddCustomEventPinsFromChildProperties(IBlueprintImporter* Importer, UK2Node_CustomEvent* EventNode, const FUObjectExport* FunctionExport)
{
	if (!Importer || !EventNode || !FunctionExport || !FunctionExport->Has(TEXT("ChildProperties"))) {
		return;
	}

	for (const FUObjectJsonValueExport& ChildValue : FunctionExport->GetArray(TEXT("ChildProperties"))) {
		const TSharedPtr<FJsonObject> ChildObject = ChildValue.JsonObject;
		if (!ChildObject.IsValid() || !ChildObject->HasField(TEXT("Name")) || !ChildObject->HasField(TEXT("PropertyFlags"))) {
			continue;
		}

		TArray<FString> PropertyFlags;
		ParseFlags(ChildObject->GetStringField(TEXT("PropertyFlags")), PropertyFlags);
		if (!HasFlag(PropertyFlags, TEXT("Parm"))) {
			continue;
		}

		FEdGraphPinType PinType;
		if (!ResolvePinType(Importer, ChildObject, PinType)) {
			continue;
		}

		EventNode->CreateUserDefinedPin(FName(*ChildObject->GetStringField(TEXT("Name"))), PinType, EGPD_Output);
	}
}
}

bool IBlueprintImporter::Import()
{
	Blueprint = Cast<UBlueprint>(CreateAsset());
	if (!Blueprint || !Blueprint->GeneratedClass) {
		return false;
	}

	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(AssetContainer, GetAssetDataAsValue());
	if (!ClassDefaultObjectExport || !ClassDefaultObjectExport->IsJsonValid() || !ClassDefaultObjectExport->GetProperties().IsValid()) {
		LogBlueprintImportWarning(FString::Printf(TEXT("Missing ClassDefaultObject for Blueprint '%s'"), *GetAssetName()), true);
		return !IsStrictBlueprintImport();
	}

	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass) {
		LogBlueprintImportWarning(FString::Printf(TEXT("Missing generated class for Blueprint '%s'"), *GetAssetName()), true);
		return false;
	}

	GetObjectSerializer()->Exports = AssetContainer->JsonObjects;

	/* Apply Blueprint object fields before graph hookup to keep inherited/base data stable first. */
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {
		"FuncMap",
		"SimpleConstructionScript",
		"ClassDefaultObject",
		"Children",
		"ChildProperties",
		"SuperStruct",
		"Super",
		"UberGraphFunction",
		"UberGraphFramePointerProperty"
	}), Blueprint);

	GetObjectSerializer()->DeserializeObjectProperties(ClassDefaultObjectExport->GetProperties(), GeneratedClass->GetDefaultObject());
	ClassDefaultObjectExport->Object = GeneratedClass;

	SetupImplementedInterfaces();
	CreateBlueprintMemberVariables(this, Blueprint, GetAssetData());
	SetupConstructionScript();
	SetupInheritableComponentHandler();
	SetupTimelineComponents();
	SetupDynamicBindings(this, Blueprint, GetAssetData());

	if (GetAssetData()->HasField(TEXT("FuncMap"))) {
		const TSharedPtr<FJsonObject> FuncMap = GetAssetData()->GetObjectField(TEXT("FuncMap"));
		ClearExistingEventNodes(Blueprint);
		UEdGraph* UberGraph = GetOrCreateUberGraph(Blueprint);

		for (const auto& Pair : FuncMap->Values) {
			const TSharedPtr<FJsonObject> FunctionObjectPath = Pair.Value->AsObject();
			if (!FunctionObjectPath.IsValid()) {
				continue;
			}

			FUObjectExport* FunctionExport = AssetContainer->GetExportByObjectPath(FunctionObjectPath);
			if (!FunctionExport || !FunctionExport->IsJsonValid()) {
				LogBlueprintImportWarning(FString::Printf(TEXT("FuncMap reference '%s' did not resolve for '%s'"), *Pair.Key, *GetAssetName()), false);
				if (IsStrictBlueprintImport()) {
					return false;
				}
				continue;
			}

			TArray<FString> FunctionFlags;
			if (FunctionExport->Has(TEXT("FunctionFlags"))) {
				ParseFlags(FunctionExport->GetString(TEXT("FunctionFlags")), FunctionFlags);
			}

			const bool bEvent = HasFlag(FunctionFlags, TEXT("FUNC_Event"));
			const bool bDelegate = HasFlag(FunctionFlags, TEXT("FUNC_Delegate"));
			const FString FunctionName = FunctionExport->GetName().ToString();

			if (!bEvent && !bDelegate) {
				UEdGraph* ExistingGraph = FindFunctionGraphByName(Blueprint, FunctionName);
				if (!ExistingGraph) {
					const FName SafeGraphName = MakeSafeGraphName(Blueprint, FunctionName, TEXT("_FuncGraph"));
					ExistingGraph = FBlueprintEditorUtils::CreateNewGraph(
						Blueprint,
						SafeGraphName,
						UEdGraph::StaticClass(),
						UEdGraphSchema_K2::StaticClass()
					);
					FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, ExistingGraph, true, nullptr);
				}

				RemoveGraphNodes(ExistingGraph);
				const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
				Schema->CreateDefaultNodesForGraph(*ExistingGraph);

				if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(ExistingGraph))) {
					EntryNode->ClearExtraFlags(FUNC_AllFlags);
					EntryNode->SetExtraFlags(ResolveFunctionFlags(FunctionFlags));
					EntryNode->Modify();
				}

				AddFunctionPinsFromChildProperties(this, Blueprint, ExistingGraph, FunctionExport);
				continue;
			}

			if (bDelegate) {
				UEdGraph* DelegateGraph = FindFunctionGraphByName(Blueprint, FunctionName);
				if (!DelegateGraph) {
					const FName SafeGraphName = MakeSafeGraphName(Blueprint, FunctionName, TEXT("_DelegateGraph"));
					DelegateGraph = FBlueprintEditorUtils::CreateNewGraph(
						Blueprint,
						SafeGraphName,
						UEdGraph::StaticClass(),
						UEdGraphSchema_K2::StaticClass()
					);
					Blueprint->DelegateSignatureGraphs.Add(DelegateGraph);
				}

				RemoveGraphNodes(DelegateGraph);
				const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
				Schema->CreateDefaultNodesForGraph(*DelegateGraph);
				Schema->CreateFunctionGraphTerminators(*DelegateGraph, static_cast<UClass*>(nullptr));
				AddFunctionPinsFromChildProperties(this, Blueprint, DelegateGraph, FunctionExport);
				continue;
			}

			if (!UberGraph) {
				LogBlueprintImportWarning(FString::Printf(TEXT("No EventGraph available for event '%s' in '%s'"), *FunctionName, *GetAssetName()), true);
				if (IsStrictBlueprintImport()) {
					return false;
				}
				continue;
			}

			UFunction* ParentFunction = nullptr;
			if (FunctionExport->Has(TEXT("SuperStruct")) && FunctionExport->GetObject(TEXT("SuperStruct")).JsonObject.IsValid()) {
				const FString SuperObjectName = FunctionExport->GetObject(TEXT("SuperStruct")).GetString(TEXT("ObjectName")).Replace(TEXT("Function'"), TEXT("")).Replace(TEXT("'"), TEXT(""));
				FString EventName;
				SuperObjectName.Split(TEXT(":"), nullptr, &EventName);
				if (!EventName.IsEmpty() && Blueprint->ParentClass) {
					ParentFunction = Blueprint->ParentClass->FindFunctionByName(*EventName);
				}
			}

			if (ParentFunction) {
				UK2Node_Event* EventNode = NewObject<UK2Node_Event>(UberGraph);
				EventNode->EventReference.SetFromField<UFunction>(ParentFunction, false);
				EventNode->bOverrideFunction = false;
				EventNode->CreateNewGuid();
				EventNode->PostPlacedNewNode();
				EventNode->AllocateDefaultPins();
				UberGraph->AddNode(EventNode, true, false);
			} else {
				UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(UberGraph);
				CustomEventNode->CustomFunctionName = *FunctionName;
				CustomEventNode->FunctionFlags = ResolveFunctionFlags(FunctionFlags);
				CustomEventNode->CreateNewGuid();
				CustomEventNode->PostPlacedNewNode();
				CustomEventNode->AllocateDefaultPins();
				AddCustomEventPinsFromChildProperties(this, CustomEventNode, FunctionExport);
				UberGraph->AddNode(CustomEventNode, true, false);
			}
		}
	} else if (UseBlueprintCompatibilityFallback() && GetAssetData()->HasTypedField<EJson::Array>(TEXT("Children"))) {
		if (IsVerboseBlueprintImport()) {
			UE_LOG(LogJsonAsAsset, Warning, TEXT("Blueprint '%s' has no FuncMap; applying compatibility fallback using Children[]"), *GetAssetName());
		}

		ClearExistingEventNodes(Blueprint);
		UEdGraph* UberGraph = GetOrCreateUberGraph(Blueprint);
		for (const TSharedPtr<FJsonValue>& ChildPathValue : GetAssetData()->GetArrayField(TEXT("Children"))) {
			const TSharedPtr<FJsonObject> ChildPathObject = ChildPathValue->AsObject();
			if (!ChildPathObject.IsValid()) {
				continue;
			}

			FUObjectExport* FunctionExport = AssetContainer->GetExportByObjectPath(ChildPathObject);
			if (!FunctionExport || !FunctionExport->IsJsonValid()) {
				continue;
			}

			const FString FunctionName = FunctionExport->GetName().ToString();
			if (FunctionName.IsEmpty()) {
				continue;
			}

			UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(UberGraph);
			CustomEventNode->CustomFunctionName = *FunctionName;
			CustomEventNode->CreateNewGuid();
			CustomEventNode->PostPlacedNewNode();
			CustomEventNode->AllocateDefaultPins();
			AddCustomEventPinsFromChildProperties(this, CustomEventNode, FunctionExport);
			UberGraph->AddNode(CustomEventNode, true, false);
		}
	} else if (IsVerboseBlueprintImport()) {
		UE_LOG(LogJsonAsAsset, Log, TEXT("Blueprint '%s' has no FuncMap; skipping function/event graph reconstruction."), *GetAssetName());
	}

	if (ShouldCompileImmediately()) {
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None);
	} else {
		UE_LOG(LogJsonAsAsset, Warning, TEXT("Skipping immediate compile for Blueprint '%s' due to BlueprintImport.CompilePolicy=%s"),
			*GetAssetName(),
			*StaticEnum<EJBlueprintCompilePolicy>()->GetNameStringByValue(static_cast<int64>(GetBlueprintImportSettings().CompilePolicy)));
	}

	return OnAssetCreation(Blueprint);
}

void IBlueprintImporter::SetupConstructionScript() const
{
	if (!GetAssetDataAsValue().Has(TEXT("SimpleConstructionScript"))) {
		return;
	}

	GetObjectSerializer()->bUseExperimentalSpawning = true;
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass) {
		return;
	}

	if (GeneratedClass->SimpleConstructionScript != nullptr) {
		MoveToTransientPackageAndRename(GeneratedClass->SimpleConstructionScript);
	}
	if (Blueprint->SimpleConstructionScript != nullptr) {
		MoveToTransientPackageAndRename(Blueprint->SimpleConstructionScript);
	}

	GeneratedClass->SimpleConstructionScript = NewObject<USimpleConstructionScript>(GeneratedClass);
	GeneratedClass->SimpleConstructionScript->SetFlags(RF_Transactional);
	Blueprint->SimpleConstructionScript = GeneratedClass->SimpleConstructionScript;

	USimpleConstructionScript* SimpleConstructionScript = GeneratedClass->SimpleConstructionScript;
	FUObjectExport* SimpleConstructionScriptExport = AssetContainer->GetExportByObjectPath(GetAssetDataAsValue().GetObject(TEXT("SimpleConstructionScript")));
	if (!SimpleConstructionScriptExport || !SimpleConstructionScriptExport->IsJsonValid()) {
		LogBlueprintImportWarning(FString::Printf(TEXT("SimpleConstructionScript export missing for '%s'"), *GetAssetName()), true);
		return;
	}

	SimpleConstructionScriptExport->Object = SimpleConstructionScript;
	GetObjectSerializer()->DeserializeObjectProperties(SimpleConstructionScriptExport->GetProperties(), SimpleConstructionScript);
	SimpleConstructionScript->FixupRootNodeParentReferences();
}

bool IBlueprintImporter::SetupInheritableComponentHandler() const
{
	if (!Blueprint || !GetAssetDataAsValue().Has(TEXT("InheritableComponentHandler"))) {
		return true;
	}

	FUObjectExport* InheritableComponentHandlerExport = AssetContainer->GetExportByObjectPath(GetAssetDataAsValue().GetObject(TEXT("InheritableComponentHandler")));
	if (!InheritableComponentHandlerExport || !InheritableComponentHandlerExport->IsJsonValid() || !InheritableComponentHandlerExport->HasProperty(TEXT("Records"))) {
		return !IsStrictBlueprintImport();
	}

	UInheritableComponentHandler* ComponentHandler = Blueprint->GetInheritableComponentHandler(true);
	if (!ComponentHandler) {
		return !IsStrictBlueprintImport();
	}

	TArray<USCS_Node*> AllSCSNodes;
	GetAllSCSNodes(Blueprint, AllSCSNodes);

	for (const TSharedPtr<FJsonValue>& RecordValue : InheritableComponentHandlerExport->GetProperties()->GetArrayField(TEXT("Records"))) {
		const TSharedPtr<FJsonObject> RecordObject = RecordValue->AsObject();
		if (!RecordObject.IsValid() || !RecordObject->HasField(TEXT("ComponentKey")) || !RecordObject->HasField(TEXT("ComponentTemplate"))) {
			continue;
		}

		const TSharedPtr<FJsonObject> ComponentKey = RecordObject->GetObjectField(TEXT("ComponentKey"));
		if (!ComponentKey.IsValid() || !ComponentKey->HasField(TEXT("SCSVariableName"))) {
			continue;
		}

		const FName SCSVariableName(*ComponentKey->GetStringField(TEXT("SCSVariableName")));
		USCS_Node* SCSNode = FindSCSNodeByVariableName(AllSCSNodes, SCSVariableName);
		if (!SCSNode) {
			if (IsVerboseBlueprintImport()) {
				UE_LOG(LogJsonAsAsset, Warning, TEXT("Inheritable component override skipped; SCS node '%s' not found"), *SCSVariableName.ToString());
			}
			continue;
		}

		const FComponentKey CreatedComponentKey(SCSNode);
		UActorComponent* CreatedTemplate = ComponentHandler->CreateOverridenComponentTemplate(CreatedComponentKey);
		if (!CreatedTemplate) {
			continue;
		}

		FUObjectExport* ComponentTemplateExport = AssetContainer->GetExportByObjectPath(RecordObject->GetObjectField(TEXT("ComponentTemplate")));
		if (!ComponentTemplateExport || !ComponentTemplateExport->IsJsonValid() || !ComponentTemplateExport->GetProperties().IsValid()) {
			continue;
		}

		GetObjectSerializer()->DeserializeObjectProperties(ComponentTemplateExport->GetProperties(), CreatedTemplate);
	}

	return true;
}

bool IBlueprintImporter::SetupImplementedInterfaces() const
{
	if (!Blueprint || !GetAssetDataAsValue().Has(TEXT("Interfaces"))) {
		return true;
	}

	Blueprint->ImplementedInterfaces.Empty();
	bool bAnyFailed = false;
	for (const TSharedPtr<FJsonValue>& InterfaceValue : GetAssetData()->GetArrayField(TEXT("Interfaces"))) {
		const TSharedPtr<FJsonObject> InterfaceObject = InterfaceValue->AsObject();
		if (!InterfaceObject.IsValid() || !InterfaceObject->HasField(TEXT("Class"))) {
			continue;
		}

		const TSharedPtr<FJsonObject> InterfaceClassObject = InterfaceObject->GetObjectField(TEXT("Class"));
		UClass* InterfaceClass = LoadClass(InterfaceClassObject);
		if (!InterfaceClass) {
			bAnyFailed = true;
			UE_LOG(LogJsonAsAsset, Warning, TEXT("Failed to load interface class '%s'"), *InterfaceClassObject->GetStringField(TEXT("ObjectPath")));
			continue;
		}

		FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetClassPathName());
	}

	return !bAnyFailed || !IsStrictBlueprintImport();
}

void IBlueprintImporter::SetupTimelineComponents() const
{
	if (!Blueprint || !GetAssetDataAsValue().Has(TEXT("Timelines"))) {
		return;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!EventGraph) {
		EventGraph = GetOrCreateUberGraph(Blueprint);
	}

	for (const TSharedPtr<FJsonValue>& TimelinePathValue : GetAssetData()->GetArrayField(TEXT("Timelines"))) {
		const TSharedPtr<FJsonObject> TimelinePathObject = TimelinePathValue->AsObject();
		if (!TimelinePathObject.IsValid()) {
			continue;
		}

		FUObjectExport* TimelineExport = AssetContainer->GetExportByObjectPath(TimelinePathObject);
		if (!TimelineExport || !TimelineExport->IsJsonValid() || !TimelineExport->GetProperties().IsValid()) {
			continue;
		}

		const TSharedPtr<FJsonObject> TimelineProperties = TimelineExport->GetProperties();
		if (!TimelineProperties.IsValid() || !TimelineProperties->HasField(TEXT("VariableName"))) {
			continue;
		}

		const FName TimelineVariableName(*TimelineProperties->GetStringField(TEXT("VariableName")));
		if (TimelineVariableName.IsNone()) {
			continue;
		}

		int32 TimelineIndex = FBlueprintEditorUtils::FindTimelineIndex(Blueprint, TimelineVariableName);
		UTimelineTemplate* TimelineTemplate = nullptr;
		if (TimelineIndex != INDEX_NONE && Blueprint->Timelines.IsValidIndex(TimelineIndex)) {
			TimelineTemplate = Blueprint->Timelines[TimelineIndex].Get();
		} else {
			TimelineTemplate = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineVariableName);
		}
		if (!TimelineTemplate) {
			continue;
		}

		if (TimelineProperties->HasField(TEXT("TimelineGuid"))) {
			TimelineTemplate->TimelineGuid = FGuid(TimelineProperties->GetStringField(TEXT("TimelineGuid")));
		}

		if (TimelineProperties->HasTypedField<EJson::Number>(TEXT("TimelineLength"))) {
			TimelineTemplate->TimelineLength = TimelineProperties->GetNumberField(TEXT("TimelineLength"));
		}
		if (TimelineProperties->HasTypedField<EJson::Boolean>(TEXT("bAutoPlay"))) {
			TimelineTemplate->bAutoPlay = TimelineProperties->GetBoolField(TEXT("bAutoPlay"));
		}
		if (TimelineProperties->HasTypedField<EJson::Boolean>(TEXT("bLoop"))) {
			TimelineTemplate->bLoop = TimelineProperties->GetBoolField(TEXT("bLoop"));
		}
		if (TimelineProperties->HasTypedField<EJson::Boolean>(TEXT("bIgnoreTimeDilation"))) {
			TimelineTemplate->bIgnoreTimeDilation = TimelineProperties->GetBoolField(TEXT("bIgnoreTimeDilation"));
		}
		if (TimelineProperties->HasTypedField<EJson::Boolean>(TEXT("bReplicated"))) {
			TimelineTemplate->bReplicated = TimelineProperties->GetBoolField(TEXT("bReplicated"));
		}

		auto ResetTrackArrays = [&]() {
			TimelineTemplate->FloatTracks.Empty();
			TimelineTemplate->VectorTracks.Empty();
			TimelineTemplate->LinearColorTracks.Empty();
			TimelineTemplate->EventTracks.Empty();
		};
		ResetTrackArrays();

		auto AddTrackCurves = [&](const FString& FieldName, FTTTrackBase::ETrackType TrackType) {
			if (!TimelineProperties->HasTypedField<EJson::Array>(*FieldName)) {
				return;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : TimelineProperties->GetArrayField(*FieldName)) {
				const TSharedPtr<FJsonObject> TrackObject = TrackValue->AsObject();
				if (!TrackObject.IsValid() || !TrackObject->HasField(TEXT("TrackName"))) {
					continue;
				}

				const FName TrackName(*TrackObject->GetStringField(TEXT("TrackName")));
				if (TrackName.IsNone()) {
					continue;
				}

				if (TrackType == FTTTrackBase::TT_FloatInterp) {
					FTTFloatTrack Track;
					Track.CurveFloat = NewObject<UCurveFloat>(Blueprint, UCurveFloat::StaticClass(), NAME_None, RF_Transactional);
					Track.SetTrackName(TrackName, TimelineTemplate);
					TimelineTemplate->FloatTracks.Add(Track);
				} else if (TrackType == FTTTrackBase::TT_VectorInterp) {
					FTTVectorTrack Track;
					Track.CurveVector = NewObject<UCurveVector>(Blueprint, UCurveVector::StaticClass(), NAME_None, RF_Transactional);
					Track.SetTrackName(TrackName, TimelineTemplate);
					TimelineTemplate->VectorTracks.Add(Track);
				} else if (TrackType == FTTTrackBase::TT_LinearColorInterp) {
					FTTLinearColorTrack Track;
					Track.CurveLinearColor = NewObject<UCurveLinearColor>(Blueprint, UCurveLinearColor::StaticClass(), NAME_None, RF_Transactional);
					Track.SetTrackName(TrackName, TimelineTemplate);
					TimelineTemplate->LinearColorTracks.Add(Track);
				} else if (TrackType == FTTTrackBase::TT_Event) {
					FTTEventTrack Track;
					Track.CurveKeys = NewObject<UCurveFloat>(Blueprint, UCurveFloat::StaticClass(), NAME_None, RF_Transactional);
					Track.SetTrackName(TrackName, TimelineTemplate);
					TimelineTemplate->EventTracks.Add(Track);
				}
			}
		};

		AddTrackCurves(TEXT("FloatTracks"), FTTTrackBase::TT_FloatInterp);
		AddTrackCurves(TEXT("VectorTracks"), FTTTrackBase::TT_VectorInterp);
		AddTrackCurves(TEXT("LinearColorTracks"), FTTTrackBase::TT_LinearColorInterp);
		AddTrackCurves(TEXT("EventTracks"), FTTTrackBase::TT_Event);

		if (EventGraph) {
			bool bTimelineNodeExists = false;
			for (UEdGraphNode* ExistingNode : EventGraph->Nodes) {
				if (UK2Node_Timeline* ExistingTimelineNode = Cast<UK2Node_Timeline>(ExistingNode)) {
					if (ExistingTimelineNode->TimelineName == TimelineVariableName) {
						bTimelineNodeExists = true;
						break;
					}
				}
			}

			if (!bTimelineNodeExists) {
				FGraphNodeCreator<UK2Node_Timeline> TimelineNodeCreator(*EventGraph);
				UK2Node_Timeline* TimelineNode = TimelineNodeCreator.CreateNode();
				TimelineNode->TimelineName = TimelineVariableName;
				TimelineNode->TimelineGuid = TimelineTemplate->TimelineGuid;
				TimelineNode->bAutoPlay = TimelineTemplate->bAutoPlay;
				TimelineNode->bLoop = TimelineTemplate->bLoop;
				TimelineNode->bReplicated = TimelineTemplate->bReplicated;
				TimelineNode->bIgnoreTimeDilation = TimelineTemplate->bIgnoreTimeDilation;
				TimelineNode->AllocateDefaultPins();
				TimelineNodeCreator.Finalize();
			}
		}
	}
}

bool IBlueprintImporter::ShouldCompileImmediately() const
{
	return ShouldCompileImmediatelyFromSettings();
}

UObject* IBlueprintImporter::CreateAsset(UObject* CreatedAsset)
{
	if (UBlueprint* ExistingBlueprint = Cast<UBlueprint>(CreatedAsset)) {
		return IImporter::CreateAsset(ExistingBlueprint);
	}

	UClass* ParentClass = LoadClass(GetSuperStructJsonObject(GetAssetData()));
	if (!ParentClass) {
		AppendNotification(
			FText::FromString("Blueprint Class Missing"),
			FText::FromString("The parent Blueprint's class could not be found. Ensure the class is defined."),
			2.0f,
			SNotificationItem::CS_Fail,
			true,
			350.0f
		);
		return nullptr;
	}

	UClass* BlueprintClass = nullptr;
	UClass* GeneratedClass = nullptr;
	FModuleManager::LoadModuleChecked<IKismetCompilerInterface>("KismetCompiler").GetBlueprintTypesForClass(
		ParentClass,
		BlueprintClass,
		GeneratedClass
	);

	UBlueprint* Existing = FindExistingBlueprint(GetPackage(), GetAssetName());
	const EJBlueprintReimportPolicy Policy = GetBlueprintImportSettings().ReimportPolicy;

	if (Existing && Policy == EJBlueprintReimportPolicy::AlwaysRecreate) {
		MoveBlueprintToTransient(Existing, TEXT("Reimport policy is AlwaysRecreate."));
		Existing = nullptr;
	}

	if (Existing && Policy == EJBlueprintReimportPolicy::RecreateInvalid && !IsBlueprintObjectValid(Existing)) {
		MoveBlueprintToTransient(Existing, TEXT("Existing Blueprint is partially constructed."));
		Existing = nullptr;
	}

	if (Existing && Policy == EJBlueprintReimportPolicy::ReuseValid && !IsBlueprintObjectValid(Existing)) {
		if (IsStrictBlueprintImport()) {
			UE_LOG(LogJsonAsAsset, Error, TEXT("ReuseValid policy prevented recreate of invalid Blueprint '%s'."), *GetAssetName());
			return nullptr;
		}

		MoveBlueprintToTransient(Existing, TEXT("ReuseValid fallback: invalid Blueprint moved out of package."));
		Existing = nullptr;
	}

	if (Existing) {
		if (Existing->GeneratedClass) {
			FBlueprintEditorUtils::PropagateParentBlueprintDefaults(Existing->GeneratedClass);
		}
		return IImporter::CreateAsset(Existing);
	}

	/* Final assert guard: ensure no conflicting object keeps this package/name. */
	if (UBlueprint* Conflicting = FindObject<UBlueprint>(GetPackage(), *GetAssetName())) {
		MoveBlueprintToTransient(Conflicting, TEXT("Conflicting object exists before CreateBlueprint."));
	}

	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		GetPackage(),
		FName(*GetAssetName()),
		GetBlueprintType(ParentClass),
		BlueprintClass,
		GeneratedClass
	);

	if (!NewBlueprint) {
		UE_LOG(LogJsonAsAsset, Error, TEXT("CreateBlueprint failed for '%s'"), *GetAssetName());
		return nullptr;
	}

	return IImporter::CreateAsset(NewBlueprint);
}
