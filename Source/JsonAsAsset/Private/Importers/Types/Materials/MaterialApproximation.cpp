/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Materials/MaterialApproximation.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture.h"
#include "Importers/Types/Materials/MaterialImporter.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Modules/Log.h"
#include "Settings/JsonAsAssetSettings.h"
#include "UObject/UObjectIterator.h"
#include "Utilities/JsonUtilities.h"

namespace {
TUniquePtr<FMaterialApproximationContext> GActiveMaterialApproximationContext;

FString NormalizeNameForRole(const FString& InName)
{
	FString Out = InName.ToLower();
	const TCHAR* Separators[] = { TEXT("_"), TEXT("-"), TEXT(" "), TEXT("+"), TEXT("."), TEXT("("), TEXT(")"), TEXT("["),
		TEXT("]") };

	for (const TCHAR* Separator : Separators) {
		Out.ReplaceInline(Separator, TEXT(""));
	}

	return Out;
}

TArray<FString> TokenizeName(const FString& InName)
{
	FString TokenSource = InName.ToLower();
	const TCHAR* Separators[] = { TEXT("_"), TEXT("-"), TEXT("+"), TEXT("."), TEXT("("), TEXT(")"), TEXT("["),
		TEXT("]") };

	for (const TCHAR* Separator : Separators) {
		TokenSource.ReplaceInline(Separator, TEXT(" "));
	}

	TArray<FString> Tokens;
	TokenSource.ParseIntoArrayWS(Tokens);
	return Tokens;
}

bool HasAny(const FString& Haystack, const TArray<FString>& Needles)
{
	for (const FString& Needle : Needles) {
		if (Haystack.Contains(Needle)) {
			return true;
		}
	}

	return false;
}

bool HasToken(const TArray<FString>& Tokens, const TArray<FString>& Needles)
{
	for (const FString& Token : Tokens) {
		for (const FString& Needle : Needles) {
			if (Token == Needle) {
				return true;
			}
		}
	}

	return false;
}

FString GetParameterName(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid()) {
		return FString();
	}

	FString Name;
	if (Object->TryGetStringField(TEXT("ParameterName"), Name)) {
		return Name;
	}

	const TSharedPtr<FJsonObject>* ParameterInfo;
	if (Object->TryGetObjectField(TEXT("ParameterInfo"), ParameterInfo) && ParameterInfo->IsValid()) {
		ParameterInfo->Get()->TryGetStringField(TEXT("Name"), Name);
	}

	return Name;
}

FString GetStringFieldSafe(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	FString Value;
	if (Object.IsValid()) {
		Object->TryGetStringField(FieldName, Value);
	}
	return Value;
}

float GetNumberFieldSafe(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const float DefaultValue)
{
	double Value = DefaultValue;
	if (Object.IsValid()) {
		Object->TryGetNumberField(FieldName, Value);
	}
	return static_cast<float>(Value);
}

bool GetBoolFieldSafe(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const bool DefaultValue)
{
	bool Value = DefaultValue;
	if (Object.IsValid()) {
		Object->TryGetBoolField(FieldName, Value);
	}
	return Value;
}

template <typename TEnum>
bool TryParseEnumValue(const FString& StringValue, TEnum& OutValue)
{
	const UEnum* Enum = StaticEnum<TEnum>();
	if (!Enum) {
		return false;
	}

	FString LocalValue = StringValue;
	if (LocalValue.Contains(TEXT("::"))) {
		LocalValue.Split(TEXT("::"), nullptr, &LocalValue, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	}

	const int64 Value = Enum->GetValueByNameString(LocalValue);
	if (Value == INDEX_NONE) {
		return false;
	}

	OutValue = static_cast<TEnum>(Value);
	return true;
}

bool TryReadBlendMode(const TSharedPtr<FJsonObject>& Object, EBlendMode& OutBlendMode)
{
	if (!Object.IsValid() || !Object->HasField(TEXT("BlendMode"))) {
		return false;
	}

	const TSharedPtr<FJsonValue> Value = Object->TryGetField(TEXT("BlendMode"));
	if (!Value.IsValid() || Value->IsNull()) {
		return false;
	}

	if (Value->Type == EJson::String) {
		return TryParseEnumValue(Value->AsString(), OutBlendMode);
	}

	if (Value->Type == EJson::Number) {
		OutBlendMode = static_cast<EBlendMode>(static_cast<int32>(Value->AsNumber()));
		return true;
	}

	return false;
}

bool TryReadShadingModel(const TSharedPtr<FJsonObject>& Object, EMaterialShadingModel& OutShadingModel)
{
	if (!Object.IsValid() || !Object->HasField(TEXT("ShadingModel"))) {
		return false;
	}

	const TSharedPtr<FJsonValue> Value = Object->TryGetField(TEXT("ShadingModel"));
	if (!Value.IsValid() || Value->IsNull()) {
		return false;
	}

	if (Value->Type == EJson::String) {
		return TryParseEnumValue(Value->AsString(), OutShadingModel);
	}

	if (Value->Type == EJson::Number) {
		OutShadingModel = static_cast<EMaterialShadingModel>(static_cast<int32>(Value->AsNumber()));
		return true;
	}

	return false;
}

FLinearColor ReadLinearColor(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid()) {
		return FLinearColor::White;
	}

	return FLinearColor(
		GetNumberFieldSafe(Object, TEXT("R"), 1.0f),
		GetNumberFieldSafe(Object, TEXT("G"), 1.0f),
		GetNumberFieldSafe(Object, TEXT("B"), 1.0f),
		GetNumberFieldSafe(Object, TEXT("A"), 1.0f)
	);
}

FString ReadObjectReferencePath(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid()) {
		return FString();
	}

	FString ObjectPath;
	if (Object->TryGetStringField(TEXT("ObjectPath"), ObjectPath)) {
		return FMaterialApproximation::NormalizeObjectPath(ObjectPath);
	}

	FString AssetPathName;
	if (Object->TryGetStringField(TEXT("AssetPathName"), AssetPathName)) {
		FString SubPathString;
		Object->TryGetStringField(TEXT("SubPathString"), SubPathString);
		if (!SubPathString.IsEmpty()) {
			if (!SubPathString.StartsWith(TEXT(":"))) {
				AssetPathName += TEXT(":");
			}
			AssetPathName += SubPathString;
		}
		return FMaterialApproximation::NormalizeObjectPath(AssetPathName);
	}

	return FString();
}

FString ObjectPathFromFileAndName(const FString& File, const FString& ExportName)
{
	FString NormalizedFile = File.Replace(TEXT("\\"), TEXT("/"));
	NormalizedFile.RemoveFromEnd(TEXT(".json"));

	FString PackagePath;
	if (NormalizedFile.Split(TEXT("/Content/"), nullptr, &PackagePath, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
		PackagePath = TEXT("/Game/") + PackagePath;
	}
	else {
		PackagePath = NormalizedFile;
	}

	FString AssetName = ExportName;
	if (AssetName.IsEmpty()) {
		PackagePath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	}

	return FMaterialApproximation::NormalizeObjectPath(PackagePath + TEXT(".") + AssetName);
}

void AddOrUpdateTexture(TArray<FApproxTextureParam>& Textures, const FApproxTextureParam& NewParam)
{
	if (NewParam.ParameterName.IsNone()) {
		return;
	}

	for (FApproxTextureParam& Existing : Textures) {
		if (Existing.ParameterName == NewParam.ParameterName) {
			if (Existing.TextureObjectPath.IsEmpty()) {
				Existing.TextureObjectPath = NewParam.TextureObjectPath;
			}
			if (Existing.TextureName.IsEmpty()) {
				Existing.TextureName = NewParam.TextureName;
			}
			if (Existing.Role == EApproxTextureRole::Unknown) {
				Existing.Role = NewParam.Role;
			}
			return;
		}
	}

	Textures.Add(NewParam);
}

void AddOrUpdateScalar(TArray<FApproxScalarParam>& Scalars, const FApproxScalarParam& NewParam)
{
	if (NewParam.ParameterName.IsNone()) {
		return;
	}

	for (FApproxScalarParam& Existing : Scalars) {
		if (Existing.ParameterName == NewParam.ParameterName) {
			if (Existing.Role == EApproxScalarRole::Unknown) {
				Existing.Role = NewParam.Role;
			}
			return;
		}
	}

	Scalars.Add(NewParam);
}

void AddOrUpdateVector(TArray<FApproxVectorParam>& Vectors, const FApproxVectorParam& NewParam)
{
	if (NewParam.ParameterName.IsNone()) {
		return;
	}

	for (FApproxVectorParam& Existing : Vectors) {
		if (Existing.ParameterName == NewParam.ParameterName) {
			if (Existing.Role == EApproxVectorRole::Unknown) {
				Existing.Role = NewParam.Role;
			}
			return;
		}
	}

	Vectors.Add(NewParam);
}

void AddOrUpdateSwitch(TArray<FApproxStaticSwitchParam>& Switches, const FApproxStaticSwitchParam& NewParam)
{
	if (NewParam.ParameterName.IsNone()) {
		return;
	}

	for (FApproxStaticSwitchParam& Existing : Switches) {
		if (Existing.ParameterName == NewParam.ParameterName) {
			return;
		}
	}

	Switches.Add(NewParam);
}

void ReadMaterialProperties(const TSharedPtr<FJsonObject>& Properties, FApproxMaterialModel& Model)
{
	if (!Properties.IsValid()) {
		return;
	}

	TryReadBlendMode(Properties, Model.BlendMode);
	TryReadShadingModel(Properties, Model.ShadingModel);
	Model.bTwoSided = GetBoolFieldSafe(Properties, TEXT("TwoSided"), Model.bTwoSided);
	Model.OpacityMaskClipValue = GetNumberFieldSafe(Properties, TEXT("OpacityMaskClipValue"), Model.OpacityMaskClipValue);

	const TSharedPtr<FJsonObject>* ShadingModels;
	if (Properties->TryGetObjectField(TEXT("ShadingModels"), ShadingModels) && ShadingModels->IsValid()) {
		double ShadingModelField = 0.0;
		if (ShadingModels->Get()->TryGetNumberField(TEXT("ShadingModelField"), ShadingModelField)) {
			Model.ShadingModelField = static_cast<int32>(ShadingModelField);
		}
	}

	const TSharedPtr<FJsonObject>* Overrides;
	if (Properties->TryGetObjectField(TEXT("BasePropertyOverrides"), Overrides) && Overrides->IsValid()) {
		TryReadBlendMode(*Overrides, Model.BlendMode);
		TryReadShadingModel(*Overrides, Model.ShadingModel);
		Model.bTwoSided = GetBoolFieldSafe(*Overrides, TEXT("TwoSided"), Model.bTwoSided);
		Model.OpacityMaskClipValue = GetNumberFieldSafe(*Overrides, TEXT("OpacityMaskClipValue"), Model.OpacityMaskClipValue);
	}
}

void ReadTextureStreamingData(const TSharedPtr<FJsonObject>& Properties, FApproxMaterialModel& Model)
{
	if (!Properties.IsValid()) {
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* TextureStreamingData;
	if (!Properties->TryGetArrayField(TEXT("TextureStreamingData"), TextureStreamingData)) {
		return;
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *TextureStreamingData) {
		const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
		if (!Entry.IsValid()) {
			continue;
		}

		FString TextureName;
		if (!Entry->TryGetStringField(TEXT("TextureName"), TextureName)) {
			continue;
		}

		FApproxTextureParam Texture;
		Texture.ParameterName = FName(*TextureName);
		Texture.TextureName = TextureName;
		Texture.Role = FMaterialApproximation::ClassifyTextureRole(TextureName);
		double UVChannel = 0.0;
		if (Entry->TryGetNumberField(TEXT("UVChannelIndex"), UVChannel)) {
			Texture.UVChannel = static_cast<int32>(UVChannel);
		}
		Texture.SamplingScale = GetNumberFieldSafe(Entry, TEXT("SamplingScale"), 1.0f);
		AddOrUpdateTexture(Model.Textures, Texture);
	}
}

void ReadRawMaterial(const TSharedPtr<FJsonObject>& Export, const FString& File, FApproxMaterialModel& Model)
{
	if (!Export.IsValid()) {
		return;
	}

	Export->TryGetStringField(TEXT("Name"), Model.MaterialName);
	if (!File.IsEmpty()) {
		Model.PackagePath = ObjectPathFromFileAndName(File, Model.MaterialName);
	}

	const TSharedPtr<FJsonObject>* Properties;
	if (Export->TryGetObjectField(TEXT("Properties"), Properties)) {
		ReadMaterialProperties(*Properties, Model);
		ReadTextureStreamingData(*Properties, Model);
	}
}

void ReadRawMaterialInstance(const TSharedPtr<FJsonObject>& Export, const FString& File, FApproxMaterialInstanceSummary& Model)
{
	if (!Export.IsValid()) {
		return;
	}

	Export->TryGetStringField(TEXT("Name"), Model.MaterialName);
	Model.PackagePath = ObjectPathFromFileAndName(File, Model.MaterialName);

	const TSharedPtr<FJsonObject>* Properties;
	if (!Export->TryGetObjectField(TEXT("Properties"), Properties) || !Properties->IsValid()) {
		return;
	}

	ReadMaterialProperties(*Properties, Model);
	ReadTextureStreamingData(*Properties, Model);

	const TSharedPtr<FJsonObject>* Parent;
	if (Properties->Get()->TryGetObjectField(TEXT("Parent"), Parent) && Parent->IsValid()) {
		const FString ParentPath = ReadObjectReferencePath(*Parent);
		if (!ParentPath.IsEmpty()) {
			Model.ParentMaterialObjectPaths.AddUnique(ParentPath);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TextureParameters;
	if (Properties->Get()->TryGetArrayField(TEXT("TextureParameterValues"), TextureParameters)) {
		for (const TSharedPtr<FJsonValue>& ParameterValue : *TextureParameters) {
			const TSharedPtr<FJsonObject> Parameter = ParameterValue.IsValid() ? ParameterValue->AsObject() : nullptr;
			const FString Name = GetParameterName(Parameter);
			if (Name.IsEmpty()) {
				continue;
			}

			const TSharedPtr<FJsonObject>* ValueObject;
			FApproxTextureParam Texture;
			Texture.ParameterName = FName(*Name);
			Texture.Role = FMaterialApproximation::ClassifyTextureRole(Name);
			if (Parameter->TryGetObjectField(TEXT("ParameterValue"), ValueObject)) {
				Texture.TextureObjectPath = ReadObjectReferencePath(*ValueObject);
				Texture.TextureName = FPackageName::ObjectPathToObjectName(Texture.TextureObjectPath);
			}
			AddOrUpdateTexture(Model.Textures, Texture);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ScalarParameters;
	if (Properties->Get()->TryGetArrayField(TEXT("ScalarParameterValues"), ScalarParameters)) {
		for (const TSharedPtr<FJsonValue>& ParameterValue : *ScalarParameters) {
			const TSharedPtr<FJsonObject> Parameter = ParameterValue.IsValid() ? ParameterValue->AsObject() : nullptr;
			const FString Name = GetParameterName(Parameter);
			if (Name.IsEmpty()) {
				continue;
			}

			FApproxScalarParam Scalar;
			Scalar.ParameterName = FName(*Name);
			Scalar.DefaultValue = GetNumberFieldSafe(Parameter, TEXT("ParameterValue"), 0.0f);
			Scalar.Role = FMaterialApproximation::ClassifyScalarRole(Name);
			AddOrUpdateScalar(Model.Scalars, Scalar);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* VectorParameters;
	if (Properties->Get()->TryGetArrayField(TEXT("VectorParameterValues"), VectorParameters)) {
		for (const TSharedPtr<FJsonValue>& ParameterValue : *VectorParameters) {
			const TSharedPtr<FJsonObject> Parameter = ParameterValue.IsValid() ? ParameterValue->AsObject() : nullptr;
			const FString Name = GetParameterName(Parameter);
			if (Name.IsEmpty()) {
				continue;
			}

			const TSharedPtr<FJsonObject>* ColorObject;
			FApproxVectorParam Vector;
			Vector.ParameterName = FName(*Name);
			Vector.Role = FMaterialApproximation::ClassifyVectorRole(Name);
			if (Parameter->TryGetObjectField(TEXT("ParameterValue"), ColorObject)) {
				Vector.DefaultValue = ReadLinearColor(*ColorObject);
			}
			AddOrUpdateVector(Model.Vectors, Vector);
		}
	}
}

void ReadSimplifiedMaterialLikeObject(const TSharedPtr<FJsonObject>& Object, const FString& File, FApproxMaterialInstanceSummary& Model)
{
	FString AssetName;
	FString NormalizedFile = File.Replace(TEXT("\\"), TEXT("/"));
	NormalizedFile.RemoveFromEnd(TEXT(".json"));
	NormalizedFile.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

	Model.MaterialName = AssetName;
	Model.PackagePath = ObjectPathFromFileAndName(File, AssetName);

	const TSharedPtr<FJsonObject>* TexturesObject;
	if (Object->TryGetObjectField(TEXT("Textures"), TexturesObject) && TexturesObject->IsValid()) {
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : TexturesObject->Get()->Values) {
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String) {
				continue;
			}

			FApproxTextureParam Texture;
			Texture.ParameterName = FName(*Pair.Key);
			Texture.TextureObjectPath = FMaterialApproximation::NormalizeObjectPath(Pair.Value->AsString());
			Texture.TextureName = FPackageName::ObjectPathToObjectName(Texture.TextureObjectPath);
			Texture.Role = FMaterialApproximation::ClassifyTextureRole(Pair.Key + TEXT(" ") + Texture.TextureName);
			AddOrUpdateTexture(Model.Textures, Texture);
		}
	}

	const TSharedPtr<FJsonObject>* ParametersObject;
	if (!Object->TryGetObjectField(TEXT("Parameters"), ParametersObject) || !ParametersObject->IsValid()) {
		return;
	}

	ReadMaterialProperties(*ParametersObject, Model);
	const TSharedPtr<FJsonObject>* NestedProperties;
	if (ParametersObject->Get()->TryGetObjectField(TEXT("Properties"), NestedProperties)) {
		ReadMaterialProperties(*NestedProperties, Model);
	}

	const TSharedPtr<FJsonObject>* ColorsObject;
	if (ParametersObject->Get()->TryGetObjectField(TEXT("Colors"), ColorsObject) && ColorsObject->IsValid()) {
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ColorsObject->Get()->Values) {
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) {
				continue;
			}

			FApproxVectorParam Vector;
			Vector.ParameterName = FName(*Pair.Key);
			Vector.DefaultValue = ReadLinearColor(Pair.Value->AsObject());
			Vector.Role = FMaterialApproximation::ClassifyVectorRole(Pair.Key);
			AddOrUpdateVector(Model.Vectors, Vector);
		}
	}

	const TSharedPtr<FJsonObject>* ScalarsObject;
	if (ParametersObject->Get()->TryGetObjectField(TEXT("Scalars"), ScalarsObject) && ScalarsObject->IsValid()) {
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ScalarsObject->Get()->Values) {
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number) {
				continue;
			}

			FApproxScalarParam Scalar;
			Scalar.ParameterName = FName(*Pair.Key);
			Scalar.DefaultValue = static_cast<float>(Pair.Value->AsNumber());
			Scalar.Role = FMaterialApproximation::ClassifyScalarRole(Pair.Key);
			AddOrUpdateScalar(Model.Scalars, Scalar);
		}
	}

	const TSharedPtr<FJsonObject>* SwitchesObject;
	if (ParametersObject->Get()->TryGetObjectField(TEXT("Switches"), SwitchesObject) && SwitchesObject->IsValid()) {
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SwitchesObject->Get()->Values) {
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Boolean) {
				continue;
			}

			FApproxStaticSwitchParam StaticSwitch;
			StaticSwitch.ParameterName = FName(*Pair.Key);
			StaticSwitch.DefaultValue = Pair.Value->AsBool();
			AddOrUpdateSwitch(Model.StaticSwitches, StaticSwitch);
		}
	}
}

template <typename TExpression>
TExpression* NewExpression(UMaterial* Material, const int32 X, const int32 Y)
{
	TExpression* Expression = NewObject<TExpression>(Material);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
#if ENGINE_UE5
	Material->GetExpressionCollection().AddExpression(Expression);
#else
	Material->Expressions.Add(Expression);
#endif
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

UMaterialExpressionTextureCoordinate* CreateTextureCoordinate(UMaterial* Material, const FApproxTextureParam& Param, const int32 X, const int32 Y)
{
	if (Param.UVChannel == 0 && FMath::IsNearlyEqual(Param.SamplingScale, 1.0f)) {
		return nullptr;
	}

	UMaterialExpressionTextureCoordinate* Coord = NewExpression<UMaterialExpressionTextureCoordinate>(Material, X, Y);
	Coord->CoordinateIndex = Param.UVChannel;
	Coord->UTiling = Param.SamplingScale;
	Coord->VTiling = Param.SamplingScale;
	return Coord;
}

FString JoinPathCandidates(const TArray<FString>& Candidates)
{
	FString Result;
	for (const FString& Candidate : Candidates) {
		if (!Result.IsEmpty()) {
			Result += TEXT(", ");
		}
		Result += Candidate;
	}
	return Result;
}

FString StripClassWrapperFromPath(const FString& InPath)
{
	FString OutPath = InPath.TrimStartAndEnd();
	const int32 FirstQuote = OutPath.Find(TEXT("'"));
	const int32 LastQuote = OutPath.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (FirstQuote != INDEX_NONE && LastQuote != INDEX_NONE && LastQuote > FirstQuote) {
		OutPath = OutPath.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
	}
	return OutPath;
}

FString EnsureGamePath(const FString& InPath)
{
	FString OutPath = StripClassWrapperFromPath(InPath).Replace(TEXT("\\"), TEXT("/"));
	if (OutPath.IsEmpty() || OutPath == TEXT("None")) {
		return FString();
	}

	if (OutPath.Contains(TEXT("/Content/"))) {
		FString Right;
		OutPath.Split(TEXT("/Content/"), nullptr, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		OutPath = TEXT("/Game/") + Right;
	}

	if (!OutPath.StartsWith(TEXT("/"))) {
		OutPath = TEXT("/") + OutPath;
	}

	return OutPath;
}

FString StripFModelNumericSuffix(const FString& InPath)
{
	FString Path = InPath;
	FString Left;
	FString Right;
	if (Path.Split(TEXT("."), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
		int32 Index = INDEX_NONE;
		if (LexTryParseString(Index, *Right)) {
			FString AssetName;
			Left.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (!AssetName.IsEmpty()) {
				return Left + TEXT(".") + AssetName;
			}
			return Left;
		}
	}
	return Path;
}

void AddTexturePathCandidate(TArray<FString>& Candidates, const FString& Candidate)
{
	if (Candidate.IsEmpty()) {
		return;
	}

	Candidates.AddUnique(Candidate);
}

void AddObjectPathForms(TArray<FString>& Candidates, const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty()) {
		return;
	}

	AddTexturePathCandidate(Candidates, ObjectPath);
	AddTexturePathCandidate(Candidates, StripFModelNumericSuffix(ObjectPath));

	FString PackagePath;
	FString ObjectName;
	if (ObjectPath.Split(TEXT("."), &PackagePath, &ObjectName, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
		if (!PackagePath.IsEmpty()) {
			FString PackageAssetName;
			PackagePath.Split(TEXT("/"), nullptr, &PackageAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

			AddTexturePathCandidate(Candidates, PackagePath + TEXT(".") + ObjectName);
			if (!PackageAssetName.IsEmpty()) {
				AddTexturePathCandidate(Candidates, PackagePath + TEXT(".") + PackageAssetName);
			}
			AddTexturePathCandidate(Candidates, PackagePath);
		}
	}
	else {
		FString PackageAssetName;
		ObjectPath.Split(TEXT("/"), nullptr, &PackageAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		AddTexturePathCandidate(Candidates, ObjectPath);
		if (!PackageAssetName.IsEmpty()) {
			AddTexturePathCandidate(Candidates, ObjectPath + TEXT(".") + PackageAssetName);
		}
	}
}

void AddTextureWrapperForms(TArray<FString>& OutCandidates, const TArray<FString>& SourceCandidates)
{
	for (const FString& Candidate : SourceCandidates) {
		AddTexturePathCandidate(OutCandidates, Candidate);
		AddTexturePathCandidate(OutCandidates, TEXT("Texture2D'") + Candidate + TEXT("'"));
	}
}

void AddNameFallbackCandidates(TArray<FString>& Candidates, const FApproxTextureParam& Param, const FString& NormalizedPath)
{
	TArray<FString> Names;
	if (!Param.TextureName.IsEmpty()) {
		Names.AddUnique(Param.TextureName);
	}
	if (!Param.ParameterName.IsNone()) {
		Names.AddUnique(Param.ParameterName.ToString());
	}

	if (!NormalizedPath.IsEmpty()) {
		const FString PathName = FPackageName::ObjectPathToObjectName(NormalizedPath);
		if (!PathName.IsEmpty()) {
			Names.AddUnique(PathName);
		}
	}

	for (FString Name : Names) {
		Name = Name.TrimStartAndEnd();
		if (Name.IsEmpty()) {
			continue;
		}

		FString Trimmed = Name;
		if (Trimmed.Contains(TEXT("."))) {
			Trimmed.Split(TEXT("."), nullptr, &Trimmed, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		}
		if (Trimmed.Contains(TEXT("/"))) {
			Trimmed.Split(TEXT("/"), nullptr, &Trimmed, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		}

		AddTexturePathCandidate(Candidates, Trimmed);
		AddTexturePathCandidate(Candidates, TEXT("/Game/") + Trimmed + TEXT(".") + Trimmed);
	}
}

TArray<FString> BuildTextureLoadCandidates(const FApproxTextureParam& Param, const FString& NormalizedPath)
{
	TArray<FString> RawCandidates;

	const FString RawGamePath = EnsureGamePath(Param.TextureObjectPath);
	const FString RawNormalized = FMaterialApproximation::NormalizeObjectPath(RawGamePath);
	const FString Normalized = FMaterialApproximation::NormalizeObjectPath(NormalizedPath);

	AddObjectPathForms(RawCandidates, RawNormalized);
	AddObjectPathForms(RawCandidates, Normalized);
	AddNameFallbackCandidates(RawCandidates, Param, Normalized);

	TArray<FString> WrappedCandidates;
	AddTextureWrapperForms(WrappedCandidates, RawCandidates);
	return WrappedCandidates;
}

UTexture* ResolveTextureByObjectName(const FString& ObjectName)
{
	if (ObjectName.IsEmpty()) {
		return nullptr;
	}

	const FName TargetName(*ObjectName);
	for (TObjectIterator<UTexture> It; It; ++It) {
		if (It->GetFName() == TargetName) {
			return *It;
		}
	}

#if WITH_EDITOR
	if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")) || FModuleManager::Get().LoadModule(TEXT("AssetRegistry")) != nullptr) {
		FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(UTexture::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		RegistryModule.Get().GetAssets(Filter, Assets);
		for (const FAssetData& AssetData : Assets) {
			if (AssetData.AssetName == TargetName) {
				if (UTexture* Texture = Cast<UTexture>(AssetData.GetAsset())) {
					return Texture;
				}
			}
		}
	}
#endif

	return nullptr;
}

UTexture* ResolveTextureForApproximation(const FApproxTextureParam& Param, const FString& NormalizedPath)
{
	const TArray<FString> Candidates = BuildTextureLoadCandidates(Param, NormalizedPath);
	UE_LOG(LogJsonAsAsset, Verbose, TEXT("[MaterialApproximation] Resolving texture param='%s' raw='%s' normalized='%s' candidates=[%s]"),
		*Param.ParameterName.ToString(), *Param.TextureObjectPath, *NormalizedPath, *JoinPathCandidates(Candidates));

	for (const FString& Candidate : Candidates) {
		UObject* LoadedObject = FindObject<UTexture>(nullptr, *Candidate);
		if (!LoadedObject) {
			LoadedObject = StaticLoadObject(UTexture::StaticClass(), nullptr, *Candidate);
		}
		if (UTexture* LoadedTexture = Cast<UTexture>(LoadedObject)) {
			UE_LOG(LogJsonAsAsset, Verbose, TEXT("[MaterialApproximation] Resolved texture param='%s' with candidate='%s'"),
				*Param.ParameterName.ToString(), *Candidate);
			return LoadedTexture;
		}
		UE_LOG(LogJsonAsAsset, Verbose, TEXT("[MaterialApproximation] Candidate failed param='%s' candidate='%s'"),
			*Param.ParameterName.ToString(), *Candidate);
	}

	TArray<FString> NameCandidates;
	if (!Param.TextureName.IsEmpty()) {
		NameCandidates.AddUnique(Param.TextureName);
	}
	if (!Param.ParameterName.IsNone()) {
		NameCandidates.AddUnique(Param.ParameterName.ToString());
	}
	if (!NormalizedPath.IsEmpty()) {
		const FString ObjectName = FPackageName::ObjectPathToObjectName(NormalizedPath);
		if (!ObjectName.IsEmpty()) {
			NameCandidates.AddUnique(ObjectName);
		}
	}

	for (const FString& NameCandidate : NameCandidates) {
		UTexture* NameResolved = ResolveTextureByObjectName(NameCandidate);
		if (NameResolved) {
			UE_LOG(LogJsonAsAsset, Verbose, TEXT("[MaterialApproximation] Resolved texture param='%s' by object name fallback='%s' path='%s'"),
				*Param.ParameterName.ToString(), *NameCandidate, *NameResolved->GetPathName());
			return NameResolved;
		}
	}

	UE_LOG(LogJsonAsAsset, Warning, TEXT("[MaterialApproximation] Failed to resolve texture param='%s' raw='%s' normalized='%s' candidates=[%s]"),
		*Param.ParameterName.ToString(), *Param.TextureObjectPath, *NormalizedPath, *JoinPathCandidates(Candidates));
	return nullptr;
}

UMaterialExpressionTextureSampleParameter2D* CreateTextureParameter(UMaterial* Material, const FApproxTextureParam& Param, const int32 X, const int32 Y)
{
	UMaterialExpressionTextureSampleParameter2D* TextureParam = NewExpression<UMaterialExpressionTextureSampleParameter2D>(Material, X, Y);
	TextureParam->ParameterName = Param.ParameterName;

	const FString NormalizedPath = FMaterialApproximation::NormalizeObjectPath(Param.TextureObjectPath);
	const bool bHasAnyTextureHint = !NormalizedPath.IsEmpty() || !Param.TextureName.IsEmpty() || !Param.ParameterName.IsNone();
	if (bHasAnyTextureHint) {
		UTexture* Texture = ResolveTextureForApproximation(Param, NormalizedPath);
		TextureParam->Texture = Texture;
		if (Texture) {
			TextureParam->SamplerType = TextureParam->GetSamplerTypeForTexture(Texture);
		}
	}

	if (UMaterialExpressionTextureCoordinate* Coord = CreateTextureCoordinate(Material, Param, X - 240, Y + 40)) {
		TextureParam->Coordinates.Connect(0, Coord);
	}

	return TextureParam;
}

UMaterialExpressionScalarParameter* CreateScalarParameter(UMaterial* Material, const FApproxScalarParam& Param, const int32 X, const int32 Y)
{
	UMaterialExpressionScalarParameter* ScalarParam = NewExpression<UMaterialExpressionScalarParameter>(Material, X, Y);
	ScalarParam->ParameterName = Param.ParameterName;
	ScalarParam->DefaultValue = Param.DefaultValue;
	return ScalarParam;
}

UMaterialExpressionVectorParameter* CreateVectorParameter(UMaterial* Material, const FApproxVectorParam& Param, const int32 X, const int32 Y)
{
	UMaterialExpressionVectorParameter* VectorParam = NewExpression<UMaterialExpressionVectorParameter>(Material, X, Y);
	VectorParam->ParameterName = Param.ParameterName;
	VectorParam->DefaultValue = Param.DefaultValue;
	return VectorParam;
}

UMaterialExpressionConstant* CreateConstant(UMaterial* Material, const float Value, const int32 X, const int32 Y)
{
	UMaterialExpressionConstant* Constant = NewExpression<UMaterialExpressionConstant>(Material, X, Y);
	Constant->R = Value;
	return Constant;
}

UMaterialExpressionComponentMask* CreateMask(UMaterial* Material, UMaterialExpression* Source, const bool R, const bool G, const bool B, const bool A,
	const int32 X, const int32 Y)
{
	UMaterialExpressionComponentMask* Mask = NewExpression<UMaterialExpressionComponentMask>(Material, X, Y);
	Mask->Input.Connect(0, Source);
	Mask->R = R;
	Mask->G = G;
	Mask->B = B;
	Mask->A = A;
	return Mask;
}

UMaterialExpression* CreateMultiply(UMaterial* Material, UMaterialExpression* A, UMaterialExpression* B, const int32 X, const int32 Y)
{
	if (!A) {
		return B;
	}
	if (!B) {
		return A;
	}

	UMaterialExpressionMultiply* Multiply = NewExpression<UMaterialExpressionMultiply>(Material, X, Y);
	Multiply->A.Connect(0, A);
	Multiply->B.Connect(0, B);
	return Multiply;
}

UMaterialExpression* CreateAdd(UMaterial* Material, UMaterialExpression* A, UMaterialExpression* B, const int32 X, const int32 Y)
{
	if (!A) {
		return B;
	}
	if (!B) {
		return A;
	}

	UMaterialExpressionAdd* Add = NewExpression<UMaterialExpressionAdd>(Material, X, Y);
	Add->A.Connect(0, A);
	Add->B.Connect(0, B);
	return Add;
}

FExpressionInput* GetMaterialInput(UMaterial* Material, const EMaterialProperty Property)
{
	return Material ? Material->GetExpressionInputForProperty(Property) : nullptr;
}

void ConnectMaterialInput(UMaterial* Material, const EMaterialProperty Property, UMaterialExpression* Expression)
{
	if (FExpressionInput* Input = GetMaterialInput(Material, Property)) {
		Input->Connect(0, Expression);
	}
}

void ApplyMaterialProperties(UMaterial* Material, const FApproxMaterialModel& Model)
{
	Material->BlendMode = Model.BlendMode;
	Material->TwoSided = Model.bTwoSided;
	Material->OpacityMaskClipValue = Model.OpacityMaskClipValue;

#if ENGINE_UE5
	if (Model.ShadingModelField != 0) {
		Material->GetShadingModels().SetShadingModelField(Model.ShadingModelField);
	}
	else {
		Material->SetShadingModel(Model.ShadingModel);
	}
#else
	Material->SetShadingModel(Model.ShadingModel);
#endif
}

void MergeModel(FApproxMaterialModel& Target, const FApproxMaterialModel& Source)
{
	for (const FApproxTextureParam& Texture : Source.Textures) {
		AddOrUpdateTexture(Target.Textures, Texture);
	}
	for (const FApproxScalarParam& Scalar : Source.Scalars) {
		AddOrUpdateScalar(Target.Scalars, Scalar);
	}
	for (const FApproxVectorParam& Vector : Source.Vectors) {
		AddOrUpdateVector(Target.Vectors, Vector);
	}
	for (const FApproxStaticSwitchParam& StaticSwitch : Source.StaticSwitches) {
		AddOrUpdateSwitch(Target.StaticSwitches, StaticSwitch);
	}
}

const FApproxTextureParam* FindTexture(const FApproxMaterialModel& Model, const TArray<EApproxTextureRole>& Roles)
{
	for (const EApproxTextureRole Role : Roles) {
		for (const FApproxTextureParam& Texture : Model.Textures) {
			if (Texture.Role == Role) {
				return &Texture;
			}
		}
	}
	return nullptr;
}

const FApproxScalarParam* FindScalar(const FApproxMaterialModel& Model, const TArray<EApproxScalarRole>& Roles)
{
	for (const EApproxScalarRole Role : Roles) {
		for (const FApproxScalarParam& Scalar : Model.Scalars) {
			if (Scalar.Role == Role) {
				return &Scalar;
			}
		}
	}
	return nullptr;
}

const FApproxVectorParam* FindVector(const FApproxMaterialModel& Model, const TArray<EApproxVectorRole>& Roles)
{
	for (const EApproxVectorRole Role : Roles) {
		for (const FApproxVectorParam& Vector : Model.Vectors) {
			if (Vector.Role == Role) {
				return &Vector;
			}
		}
	}
	return nullptr;
}

const FApproxTextureParam* FindFallbackBaseColorTexture(const FApproxMaterialModel& Model)
{
	const FApproxTextureParam* Candidate = nullptr;
	int32 CandidateCount = 0;

	for (const FApproxTextureParam& Texture : Model.Textures) {
		const bool bExcluded =
			Texture.Role == EApproxTextureRole::Normal ||
			Texture.Role == EApproxTextureRole::ORM ||
			Texture.Role == EApproxTextureRole::Emissive ||
			Texture.Role == EApproxTextureRole::EmissiveBlinkers ||
			Texture.Role == EApproxTextureRole::Opacity ||
			Texture.Role == EApproxTextureRole::OpacityMask;

		if (bExcluded) {
			continue;
		}

		Candidate = &Texture;
		CandidateCount++;
		if (CandidateCount > 1) {
			return nullptr;
		}
	}

	return CandidateCount == 1 ? Candidate : nullptr;
}

bool HasGraphInputs(const FApproxMaterialModel& Model)
{
	return Model.Textures.Num() > 0 || Model.Scalars.Num() > 0 || Model.Vectors.Num() > 0 || Model.StaticSwitches.Num() > 0;
}

bool GenerateGraph(UMaterial* Material, const FApproxMaterialModel& Model)
{
	if (!Material || !HasGraphInputs(Model)) {
		return false;
	}

	const UJsonAsAssetSettings* Settings = GetSettings();
	const bool bCreateUnknownNodes = Settings && Settings->AssetSettings.Material.CreateUnconnectedUnknownParameterNodes;
	const int32 MaxUnknownTextureNodes = 48;
	const int32 MaxUnknownScalarNodes = 48;
	const int32 MaxUnknownVectorNodes = 48;
	int32 UnknownTextureCount = 0;
	int32 UnknownScalarCount = 0;
	int32 UnknownVectorCount = 0;
	ApplyMaterialProperties(Material, Model);

	TMap<FName, UMaterialExpressionTextureSampleParameter2D*> TextureNodes;
	TMap<FName, UMaterialExpressionScalarParameter*> ScalarNodes;
	TMap<FName, UMaterialExpressionVectorParameter*> VectorNodes;

	int32 Y = -480;
	for (const FApproxTextureParam& Texture : Model.Textures) {
		if (Texture.Role == EApproxTextureRole::Unknown) {
			UnknownTextureCount++;
		}
		if (Texture.Role == EApproxTextureRole::Unknown && (!bCreateUnknownNodes || UnknownTextureCount > MaxUnknownTextureNodes)) {
			continue;
		}
		TextureNodes.Add(Texture.ParameterName, CreateTextureParameter(Material, Texture, -960, Y));
		Y += 220;
	}

	Y = -480;
	for (const FApproxVectorParam& Vector : Model.Vectors) {
		if (Vector.Role == EApproxVectorRole::Unknown) {
			UnknownVectorCount++;
		}
		if (Vector.Role == EApproxVectorRole::Unknown && (!bCreateUnknownNodes || UnknownVectorCount > MaxUnknownVectorNodes)) {
			continue;
		}
		VectorNodes.Add(Vector.ParameterName, CreateVectorParameter(Material, Vector, -960, Y));
		Y += 160;
	}

	Y = -480;
	for (const FApproxScalarParam& Scalar : Model.Scalars) {
		if (Scalar.Role == EApproxScalarRole::Unknown) {
			UnknownScalarCount++;
		}
		if (Scalar.Role == EApproxScalarRole::Unknown && (!bCreateUnknownNodes || UnknownScalarCount > MaxUnknownScalarNodes)) {
			continue;
		}
		ScalarNodes.Add(Scalar.ParameterName, CreateScalarParameter(Material, Scalar, -720, Y));
		Y += 120;
	}

	if (bCreateUnknownNodes) {
		if (UnknownTextureCount > MaxUnknownTextureNodes) {
			UE_LOG(LogJsonAsAsset, Warning, TEXT("[MaterialApproximation] Truncated unknown texture params (%d -> %d) for material '%s'."),
				UnknownTextureCount, MaxUnknownTextureNodes, *Model.MaterialName);
		}
		if (UnknownScalarCount > MaxUnknownScalarNodes) {
			UE_LOG(LogJsonAsAsset, Warning, TEXT("[MaterialApproximation] Truncated unknown scalar params (%d -> %d) for material '%s'."),
				UnknownScalarCount, MaxUnknownScalarNodes, *Model.MaterialName);
		}
		if (UnknownVectorCount > MaxUnknownVectorNodes) {
			UE_LOG(LogJsonAsAsset, Warning, TEXT("[MaterialApproximation] Truncated unknown vector params (%d -> %d) for material '%s'."),
				UnknownVectorCount, MaxUnknownVectorNodes, *Model.MaterialName);
		}
	}

	if (bCreateUnknownNodes) {
		Y = 320;
		for (const FApproxStaticSwitchParam& StaticSwitch : Model.StaticSwitches) {
			FApproxScalarParam ScalarProxy;
			ScalarProxy.ParameterName = StaticSwitch.ParameterName;
			ScalarProxy.DefaultValue = StaticSwitch.DefaultValue ? 1.0f : 0.0f;

			UMaterialExpressionConstant* TrueValue = CreateConstant(Material, 1.0f, -960, Y);
			UMaterialExpressionConstant* FalseValue = CreateConstant(Material, 0.0f, -960, Y + 60);
			UMaterialExpressionStaticSwitchParameter* SwitchParam = NewExpression<UMaterialExpressionStaticSwitchParameter>(Material, -720, Y);
			SwitchParam->ParameterName = StaticSwitch.ParameterName;
			SwitchParam->DefaultValue = StaticSwitch.DefaultValue;
			SwitchParam->A.Connect(0, TrueValue);
			SwitchParam->B.Connect(0, FalseValue);
			Y += 160;
		}
	}

	const bool bUnlitLike = Material->MaterialDomain == MD_UI || Material->MaterialDomain == MD_LightFunction ||
#if ENGINE_UE5
		Material->GetShadingModels().HasShadingModel(MSM_Unlit);
#else
		Material->ShadingModel == MSM_Unlit;
#endif

	UMaterialExpression* BaseColorOutput = nullptr;
	if (const FApproxTextureParam* BaseColor = FindTexture(Model, { EApproxTextureRole::BaseColor })) {
		BaseColorOutput = TextureNodes.FindRef(BaseColor->ParameterName);
	}
	else if (const FApproxTextureParam* FallbackBaseColor = FindFallbackBaseColorTexture(Model)) {
		BaseColorOutput = TextureNodes.FindRef(FallbackBaseColor->ParameterName);
		UE_LOG(LogJsonAsAsset, Verbose, TEXT("[MaterialApproximation] Using fallback base color texture param='%s' role=%d"),
			*FallbackBaseColor->ParameterName.ToString(), static_cast<int32>(FallbackBaseColor->Role));
	}

	if (const FApproxVectorParam* Tint = FindVector(Model, { EApproxVectorRole::TintColor })) {
		UMaterialExpression* TintNode = VectorNodes.FindRef(Tint->ParameterName);
		BaseColorOutput = BaseColorOutput ? CreateMultiply(Material, BaseColorOutput, TintNode, -420, -420) : TintNode;
	}

	if (BaseColorOutput && !bUnlitLike) {
		ConnectMaterialInput(Material, MP_BaseColor, BaseColorOutput);
	}

	if (const FApproxTextureParam* Normal = FindTexture(Model, { EApproxTextureRole::Normal })) {
		ConnectMaterialInput(Material, MP_Normal, TextureNodes.FindRef(Normal->ParameterName));
	}

	if (const FApproxTextureParam* ORM = FindTexture(Model, { EApproxTextureRole::ORM })) {
		UMaterialExpression* ORMNode = TextureNodes.FindRef(ORM->ParameterName);
		ConnectMaterialInput(Material, MP_AmbientOcclusion, CreateMask(Material, ORMNode, true, false, false, false, -420, -120));
		ConnectMaterialInput(Material, MP_Roughness, CreateMask(Material, ORMNode, false, true, false, false, -420, 0));
		ConnectMaterialInput(Material, MP_Metallic, CreateMask(Material, ORMNode, false, false, true, false, -420, 120));
	}
	else {
		if (const FApproxTextureParam* RoughnessTexture = FindTexture(Model, { EApproxTextureRole::Roughness })) {
			ConnectMaterialInput(Material, MP_Roughness, TextureNodes.FindRef(RoughnessTexture->ParameterName));
		}
		else if (const FApproxScalarParam* RoughnessScalar = FindScalar(Model, { EApproxScalarRole::Roughness })) {
			ConnectMaterialInput(Material, MP_Roughness, ScalarNodes.FindRef(RoughnessScalar->ParameterName));
		}

		if (const FApproxTextureParam* MetallicTexture = FindTexture(Model, { EApproxTextureRole::Metallic })) {
			ConnectMaterialInput(Material, MP_Metallic, TextureNodes.FindRef(MetallicTexture->ParameterName));
		}
		else if (const FApproxScalarParam* MetallicScalar = FindScalar(Model, { EApproxScalarRole::Metallic })) {
			ConnectMaterialInput(Material, MP_Metallic, ScalarNodes.FindRef(MetallicScalar->ParameterName));
		}

		if (const FApproxTextureParam* AOTexture = FindTexture(Model, { EApproxTextureRole::AmbientOcclusion })) {
			ConnectMaterialInput(Material, MP_AmbientOcclusion, TextureNodes.FindRef(AOTexture->ParameterName));
		}
	}

	UMaterialExpression* EmissiveOutput = nullptr;
	if (const FApproxTextureParam* Emissive = FindTexture(Model, { EApproxTextureRole::Emissive })) {
		EmissiveOutput = TextureNodes.FindRef(Emissive->ParameterName);
	}
	if (const FApproxTextureParam* Blinker = FindTexture(Model, { EApproxTextureRole::EmissiveBlinkers })) {
		EmissiveOutput = CreateAdd(Material, EmissiveOutput, TextureNodes.FindRef(Blinker->ParameterName), -420, 320);
	}
	if (const FApproxVectorParam* EmissiveColor = FindVector(Model, { EApproxVectorRole::EmissiveColor })) {
		EmissiveOutput = CreateMultiply(Material, EmissiveOutput, VectorNodes.FindRef(EmissiveColor->ParameterName), -220, 280);
	}
	if (const FApproxScalarParam* EmissiveIntensity = FindScalar(Model, { EApproxScalarRole::EmissiveIntensity })) {
		EmissiveOutput = CreateMultiply(Material, EmissiveOutput, ScalarNodes.FindRef(EmissiveIntensity->ParameterName), -220, 360);
	}
	if (!EmissiveOutput && bUnlitLike) {
		EmissiveOutput = BaseColorOutput;
	}
	if (EmissiveOutput) {
		ConnectMaterialInput(Material, MP_EmissiveColor, EmissiveOutput);
	}

	const FApproxTextureParam* OpacityTexture = FindTexture(Model, { EApproxTextureRole::OpacityMask, EApproxTextureRole::Opacity });
	const FApproxScalarParam* OpacityScalar = FindScalar(Model, { EApproxScalarRole::Opacity });
	UMaterialExpression* OpacityExpression = nullptr;
	if (OpacityTexture) {
		OpacityExpression = TextureNodes.FindRef(OpacityTexture->ParameterName);
	}
	else if (OpacityScalar) {
		OpacityExpression = ScalarNodes.FindRef(OpacityScalar->ParameterName);
	}

	if (OpacityExpression) {
		if (Model.BlendMode == BLEND_Translucent || Model.BlendMode == BLEND_AlphaComposite || Model.BlendMode == BLEND_AlphaHoldout) {
			ConnectMaterialInput(Material, MP_Opacity, OpacityExpression);
		}
		else {
			Material->BlendMode = BLEND_Masked;
			ConnectMaterialInput(Material, MP_OpacityMask, OpacityExpression);
		}
	}

	if (Model.ShadingModel == MSM_ClearCoat) {
#if ENGINE_UE5
		Material->SetShadingModel(MSM_ClearCoat);
#endif
		if (const FApproxScalarParam* ClearCoat = FindScalar(Model, { EApproxScalarRole::ClearCoat })) {
			ConnectMaterialInput(Material, MP_CustomData0, ScalarNodes.FindRef(ClearCoat->ParameterName));
		}
		else {
			ConnectMaterialInput(Material, MP_CustomData0, CreateConstant(Material, 1.0f, -420, 520));
		}

		if (const FApproxScalarParam* ClearCoatRoughness = FindScalar(Model, { EApproxScalarRole::ClearCoatRoughness, EApproxScalarRole::Roughness })) {
			ConnectMaterialInput(Material, MP_CustomData1, ScalarNodes.FindRef(ClearCoatRoughness->ParameterName));
		}
		else {
			ConnectMaterialInput(Material, MP_CustomData1, CreateConstant(Material, 0.08f, -420, 640));
		}
	}

	return true;
}

bool DeserializeJsonFile(const FString& JsonFile, TArray<TSharedPtr<FJsonValue>>& OutArray, TSharedPtr<FJsonObject>& OutObject)
{
	OutArray.Reset();
	OutObject.Reset();

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *JsonFile)) {
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	TSharedPtr<FJsonValue> RootValue;
	if (!FJsonSerializer::Deserialize(Reader, RootValue) || !RootValue.IsValid()) {
		return false;
	}

	if (RootValue->Type == EJson::Array) {
		OutArray = RootValue->AsArray();
		return true;
	}

	if (RootValue->Type == EJson::Object) {
		OutObject = RootValue->AsObject();
		return OutObject.IsValid();
	}

	return false;
}
}

FScopedMaterialApproximationImportSession::FScopedMaterialApproximationImportSession(const TArray<FString>& JsonFiles)
{
	FMaterialApproximation::BeginImportSession(JsonFiles);
}

FScopedMaterialApproximationImportSession::~FScopedMaterialApproximationImportSession()
{
	FMaterialApproximation::EndImportSession();
}

EApproxTextureRole FMaterialApproximation::ClassifyTextureRole(const FString& Name)
{
	const FString Normalized = NormalizeNameForRole(Name);
	const TArray<FString> Tokens = TokenizeName(Name);

	if (HasAny(Normalized, { TEXT("blinker"), TEXT("blinkers"), TEXT("indicator"), TEXT("turnsignal"), TEXT("signal") })) {
		return EApproxTextureRole::EmissiveBlinkers;
	}
	if (HasAny(Normalized, { TEXT("normal"), TEXT("normals"), TEXT("normalmap") }) || HasToken(Tokens, { TEXT("n"), TEXT("nor") })) {
		return EApproxTextureRole::Normal;
	}
	if (HasAny(Normalized, { TEXT("opacitymask"), TEXT("alphamask"), TEXT("opacity"), TEXT("transparent"), TEXT("transparency") }) ||
		HasToken(Tokens, { TEXT("alpha") })) {
		return EApproxTextureRole::OpacityMask;
	}
	if (HasAny(Normalized, { TEXT("orm"), TEXT("arm"), TEXT("aorm"), TEXT("occlusionroughnessmetallic"), TEXT("occlusionroughnessmetallicmask"),
		TEXT("aormmask"), TEXT("armmask"), TEXT("ormmask"), TEXT("specularmask"), TEXT("specularmasks"), TEXT("packed"), TEXT("packedmask") })) {
		return EApproxTextureRole::ORM;
	}
	if (HasAny(Normalized, { TEXT("basecolor"), TEXT("basecolour"), TEXT("albedo"), TEXT("diffuse"), TEXT("colour"), TEXT("color") }) ||
		HasToken(Tokens, { TEXT("diff") })) {
		return EApproxTextureRole::BaseColor;
	}
	if (HasAny(Normalized, { TEXT("roughness") }) || HasToken(Tokens, { TEXT("rough"), TEXT("rgh") })) {
		return EApproxTextureRole::Roughness;
	}
	if (HasAny(Normalized, { TEXT("metallic"), TEXT("metalness"), TEXT("metalic") }) || HasToken(Tokens, { TEXT("metal") })) {
		return EApproxTextureRole::Metallic;
	}
	if (HasAny(Normalized, { TEXT("ambientocclusion"), TEXT("occlusion") }) || HasToken(Tokens, { TEXT("ao") })) {
		return EApproxTextureRole::AmbientOcclusion;
	}
	if (HasAny(Normalized, { TEXT("emissive"), TEXT("emission"), TEXT("glow") })) {
		return EApproxTextureRole::Emissive;
	}
	if (HasAny(Normalized, { TEXT("specular") })) {
		return EApproxTextureRole::Specular;
	}
	if (HasAny(Normalized, { TEXT("clearcoatroughness") })) {
		return EApproxTextureRole::ClearCoatRoughness;
	}
	if (HasAny(Normalized, { TEXT("clearcoat") })) {
		return EApproxTextureRole::ClearCoat;
	}

	return EApproxTextureRole::Unknown;
}

EApproxScalarRole FMaterialApproximation::ClassifyScalarRole(const FString& Name)
{
	const FString Normalized = NormalizeNameForRole(Name);

	if (HasAny(Normalized, { TEXT("clearcoatroughness"), TEXT("roughnessclearcoat"), TEXT("roughclearcoat") })) {
		return EApproxScalarRole::ClearCoatRoughness;
	}
	if (HasAny(Normalized, { TEXT("clearcoat"), TEXT("clearcoatamount") })) {
		return EApproxScalarRole::ClearCoat;
	}
	if (HasAny(Normalized, { TEXT("emissiveintensity"), TEXT("emissivepower"), TEXT("glowintensity"), TEXT("brightness") }) ||
		Normalized == TEXT("lights")) {
		return EApproxScalarRole::EmissiveIntensity;
	}
	if (HasAny(Normalized, { TEXT("roughness"), TEXT("paintroughness") })) {
		return EApproxScalarRole::Roughness;
	}
	if (HasAny(Normalized, { TEXT("metallic"), TEXT("metalness"), TEXT("metalic") })) {
		return EApproxScalarRole::Metallic;
	}
	if (HasAny(Normalized, { TEXT("opacity"), TEXT("alpha"), TEXT("transparent") })) {
		return EApproxScalarRole::Opacity;
	}
	if (HasAny(Normalized, { TEXT("ambientocclusion"), TEXT("occlusion") }) || Normalized == TEXT("ao")) {
		return EApproxScalarRole::AmbientOcclusion;
	}

	return EApproxScalarRole::Unknown;
}

EApproxVectorRole FMaterialApproximation::ClassifyVectorRole(const FString& Name)
{
	const FString Normalized = NormalizeNameForRole(Name);

	if (HasAny(Normalized, { TEXT("emissive"), TEXT("emission"), TEXT("glow") })) {
		return EApproxVectorRole::EmissiveColor;
	}
	if (HasAny(Normalized, { TEXT("paintcolor"), TEXT("paintcolour"), TEXT("tint"), TEXT("color"), TEXT("colour") })) {
		return EApproxVectorRole::TintColor;
	}

	return EApproxVectorRole::Unknown;
}

FString FMaterialApproximation::NormalizeObjectPath(const FString& ObjectPath)
{
	FString Path = StripClassWrapperFromPath(ObjectPath).Replace(TEXT("\\"), TEXT("/"));

	if (Path.IsEmpty() || Path == TEXT("None")) {
		return FString();
	}

	if (Path.Contains(TEXT(":"))) {
		Path.Split(TEXT(":"), &Path, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	}

	if (Path.Contains(TEXT("/Content/"))) {
		Path.Split(TEXT("/Content/"), nullptr, &Path, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		Path = TEXT("/Game/") + Path;
	}
	else if (Path.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase)) {
		Path.RightChopInline(8, EAllowShrinking::No);
		Path = TEXT("/Game/") + Path;
	}

	if (!Path.StartsWith(TEXT("/"))) {
		Path = TEXT("/") + Path;
	}

	FString PackagePath = Path;
	FString ObjectName;
	if (Path.Split(TEXT("."), &PackagePath, &ObjectName, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
		int32 NumericExportIndex = INDEX_NONE;
		if (LexTryParseString(NumericExportIndex, *ObjectName)) {
			FString PackageAssetName;
			PackagePath.Split(TEXT("/"), nullptr, &PackageAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			return PackagePath + TEXT(".") + PackageAssetName;
		}
		return PackagePath + TEXT(".") + ObjectName;
	}

	FString PackageAssetName;
	Path.Split(TEXT("/"), nullptr, &PackageAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	return Path + TEXT(".") + PackageAssetName;
}

int32 FMaterialApproximation::GetImportRankForFile(const FString& JsonFile)
{
	const FString NormalizedFile = JsonFile.Replace(TEXT("\\"), TEXT("/"));
	if (GActiveMaterialApproximationContext.IsValid()) {
		if (const int32* Rank = GActiveMaterialApproximationContext->ImportRankByFile.Find(NormalizedFile)) {
			return *Rank;
		}
	}

	return 2;
}

void FMaterialApproximation::BeginImportSession(const TArray<FString>& JsonFiles)
{
	GActiveMaterialApproximationContext = BuildContext(JsonFiles);
}

void FMaterialApproximation::EndImportSession()
{
	GActiveMaterialApproximationContext.Reset();
}

const FMaterialApproximationContext* FMaterialApproximation::GetActiveContext()
{
	return GActiveMaterialApproximationContext.Get();
}

bool FMaterialApproximation::TryApproximateMaterial(IMaterialImporter* MaterialImporter, const TSharedPtr<FJsonObject>& MaterialProperties)
{
	if (!MaterialImporter) {
		return false;
	}

	UMaterial* Material = MaterialImporter->GetTypedAsset<UMaterial>();
	if (!Material) {
		return false;
	}

	FApproxMaterialModel Model;
	Model.MaterialName = MaterialImporter->GetAssetName();
	Model.PackagePath = MaterialImporter->GetPackage() ? NormalizeObjectPath(MaterialImporter->GetPackage()->GetName() + TEXT(".") + Model.MaterialName) : FString();
	ReadMaterialProperties(MaterialProperties, Model);
	ReadTextureStreamingData(MaterialProperties, Model);
	ReadRawMaterial(MaterialImporter->GetAssetExport(), FString(), Model);

	const UJsonAsAssetSettings* Settings = GetSettings();
	const bool bUseSiblingContext = Settings == nullptr || Settings->AssetSettings.Material.UseSiblingMaterialInstancesForApproximation;
	if (bUseSiblingContext && GActiveMaterialApproximationContext.IsValid()) {
		TArray<FString> CandidatePaths;
		CandidatePaths.AddUnique(NormalizeObjectPath(Model.PackagePath));
		if (MaterialImporter->GetPackage()) {
			CandidatePaths.AddUnique(NormalizeObjectPath(MaterialImporter->GetPackage()->GetName()));
			CandidatePaths.AddUnique(NormalizeObjectPath(MaterialImporter->GetPackage()->GetName() + TEXT(".") + Model.MaterialName));
		}

		for (const FString& CandidatePath : CandidatePaths) {
			if (const TArray<FApproxMaterialInstanceSummary>* Children = GActiveMaterialApproximationContext->ChildrenByParentObjectPath.Find(CandidatePath)) {
				for (const FApproxMaterialInstanceSummary& Child : *Children) {
					MergeModel(Model, Child);
				}
			}
		}
	}

	if (!HasGraphInputs(Model) && !(Settings && Settings->AssetSettings.Material.GenerateGenericParametersWhenNoInstancesFound)) {
		return false;
	}

	if (!HasGraphInputs(Model)) {
		return false;
	}

	const bool bGenerated = GenerateGraph(Material, Model);
	if (bGenerated) {
		UE_LOG(LogJsonAsAsset, Log, TEXT("[MaterialApproximation] Generated heuristic material graph for '%s'."), *MaterialImporter->GetAssetName());
	}

	return bGenerated;
}

TUniquePtr<FMaterialApproximationContext> FMaterialApproximation::BuildContext(const TArray<FString>& JsonFiles)
{
	TUniquePtr<FMaterialApproximationContext> Context = MakeUnique<FMaterialApproximationContext>();

	for (const FString& JsonFile : JsonFiles) {
		const FString NormalizedFile = JsonFile.Replace(TEXT("\\"), TEXT("/"));
		TArray<TSharedPtr<FJsonValue>> Exports;
		TSharedPtr<FJsonObject> ObjectRoot;
		if (!DeserializeJsonFile(NormalizedFile, Exports, ObjectRoot)) {
			Context->ImportRankByFile.Add(NormalizedFile, 2);
			continue;
		}

		int32 Rank = 2;
		for (const TSharedPtr<FJsonValue>& ExportValue : Exports) {
			const TSharedPtr<FJsonObject> Export = ExportValue.IsValid() ? ExportValue->AsObject() : nullptr;
			if (!Export.IsValid()) {
				continue;
			}

			const FString Type = GetStringFieldSafe(Export, TEXT("Type"));
			if (Type == TEXT("Material")) {
				Rank = FMath::Min(Rank, 0);
				FApproxMaterialModel Model;
				ReadRawMaterial(Export, NormalizedFile, Model);
				Context->MaterialsByObjectPath.Add(NormalizeObjectPath(Model.PackagePath), Model);
			}
			else if (Type == TEXT("MaterialInstanceConstant")) {
				Rank = FMath::Min(Rank, 1);
				FApproxMaterialInstanceSummary Summary;
				ReadRawMaterialInstance(Export, NormalizedFile, Summary);
				for (const FString& ParentPath : Summary.ParentMaterialObjectPaths) {
					Context->ChildrenByParentObjectPath.FindOrAdd(NormalizeObjectPath(ParentPath)).Add(Summary);
				}
			}
		}

		if (ObjectRoot.IsValid() && ObjectRoot->HasField(TEXT("Type"))) {
			const FString Type = GetStringFieldSafe(ObjectRoot, TEXT("Type"));
			if (Type == TEXT("Material")) {
				Rank = FMath::Min(Rank, 0);
				FApproxMaterialModel Model;
				ReadRawMaterial(ObjectRoot, NormalizedFile, Model);
				Context->MaterialsByObjectPath.Add(NormalizeObjectPath(Model.PackagePath), Model);
			}
			else if (Type == TEXT("MaterialInstanceConstant")) {
				Rank = FMath::Min(Rank, 1);
				FApproxMaterialInstanceSummary Summary;
				ReadRawMaterialInstance(ObjectRoot, NormalizedFile, Summary);
				for (const FString& ParentPath : Summary.ParentMaterialObjectPaths) {
					Context->ChildrenByParentObjectPath.FindOrAdd(NormalizeObjectPath(ParentPath)).Add(Summary);
				}
			}
		}

		if (ObjectRoot.IsValid() && ObjectRoot->HasField(TEXT("Textures")) && ObjectRoot->HasField(TEXT("Parameters"))) {
			FString AssetName;
			NormalizedFile.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			AssetName.RemoveFromEnd(TEXT(".json"));

			FApproxMaterialInstanceSummary Summary;
			ReadSimplifiedMaterialLikeObject(ObjectRoot, NormalizedFile, Summary);

			if (AssetName.StartsWith(TEXT("M_"))) {
				Rank = FMath::Min(Rank, 0);
				Context->MaterialsByObjectPath.Add(NormalizeObjectPath(Summary.PackagePath), Summary);
			}
			else {
				Rank = FMath::Min(Rank, 1);
			}
		}

		Context->ImportRankByFile.Add(NormalizedFile, Rank);
	}

	return Context;
}
