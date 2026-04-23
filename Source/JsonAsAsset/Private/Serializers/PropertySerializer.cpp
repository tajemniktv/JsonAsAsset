/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Serializers/PropertySerializer.h"

#include "GameplayTagContainer.h"
#include "Importers/Constructor/Importer.h"
#include "Serializers/ObjectSerializer.h"
#include "UObject/TextProperty.h"
#if ENGINE_UE5
#include "UObject/PropertyOptional.h"
#endif

/* Struct Serializers */
#include "Decooking/ParticleSystemDecooking.h"
#include "Distributions.h"
#include "Engine/FontFace.h"
#include "MovieSceneSection.h"
#include "Serializers/Structs/DateTimeSerializer.h"
#include "Serializers/Structs/FallbackStructSerializer.h"
#include "Serializers/Structs/TimeSpanSerializer.h"

#if ENGINE_UE4
#include "Settings/Runtime.h"
#endif

DECLARE_LOG_CATEGORY_CLASS(LogJsonAsAssetPropertySerializer, Error, Log);
UE_DISABLE_OPTIMIZATION

UPropertySerializer::UPropertySerializer() {
  FallbackStructSerializer = MakeShared<FFallbackStructSerializer>(this);

  UScriptStruct *DateTimeStruct =
      FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.DateTime"));
  UScriptStruct *TimespanStruct =
      FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.TimeSpan"));
  check(DateTimeStruct);
  check(TimespanStruct);

  StructSerializers.Add(DateTimeStruct, MakeShared<FDateTimeSerializer>());
  StructSerializers.Add(TimespanStruct, MakeShared<FTimeSpanSerializer>());
}

void UPropertySerializer::DeserializePropertyValue(
    FProperty *Property, const TSharedRef<FJsonValue> &JsonValue,
    void *OutValue) {
  const FMapProperty *MapProperty = CastField<const FMapProperty>(Property);
  const FSetProperty *SetProperty = CastField<const FSetProperty>(Property);
  const FArrayProperty *ArrayProperty =
      CastField<const FArrayProperty>(Property);

  TSharedRef<FJsonValue> NewJsonValue = JsonValue;

  if (BlacklistedPropertyNames.Contains(Property->GetName())) {
    return;
  }

  if (MapProperty) {
    if (NewJsonValue->IsNull()) {
      return;
    }

    FProperty *KeyProperty = MapProperty->KeyProp;
    FProperty *ValueProperty = MapProperty->ValueProp;
    FScriptMapHelper MapHelper(MapProperty, OutValue);
    const TArray<TSharedPtr<FJsonValue>> &PairArray = NewJsonValue->AsArray();

    for (int32 i = 0; i < PairArray.Num(); i++) {
      const TSharedPtr<FJsonObject> &Pair = PairArray[i]->AsObject();
      if (!Pair.IsValid()) {
        UE_LOG(LogJsonAsAssetPropertySerializer, Warning,
               TEXT("Skipping invalid map pair object while deserializing "
                    "property '%s'."),
               *Property->GetName());
        continue;
      }

      const TSharedPtr<FJsonValue> *EntryKeyPtr =
          Pair->Values.Find(TEXT("Key"));
      const TSharedPtr<FJsonValue> *EntryValuePtr =
          Pair->Values.Find(TEXT("Value"));
      if (EntryKeyPtr == nullptr || EntryValuePtr == nullptr ||
          !EntryKeyPtr->IsValid() || !EntryValuePtr->IsValid()) {
        UE_LOG(LogJsonAsAssetPropertySerializer, Warning,
               TEXT("Skipping map pair with missing key/value while "
                    "deserializing property '%s'."),
               *Property->GetName());
        continue;
      }

      const TSharedPtr<FJsonValue> &EntryKey = *EntryKeyPtr;
      const TSharedPtr<FJsonValue> &EntryValue = *EntryValuePtr;
      const int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
      uint8 *PairPtr = MapHelper.GetPairPtr(Index);

      /* Copy over imported key and value from temporary storage */
      DeserializePropertyValue(KeyProperty, EntryKey.ToSharedRef(), PairPtr);
      DeserializePropertyValue(ValueProperty, EntryValue.ToSharedRef(),
                               PairPtr + MapHelper.MapLayout.ValueOffset);
    }
    MapHelper.Rehash();

  } else if (SetProperty) {
    FProperty *ElementProperty = SetProperty->ElementProp;
    FScriptSetHelper SetHelper(SetProperty, OutValue);
    const TArray<TSharedPtr<FJsonValue>> &SetArray = NewJsonValue->AsArray();
    SetHelper.EmptyElements();
    uint8 *TempElementStorage =
        static_cast<uint8 *>(FMemory::Malloc(ElementProperty->GetElementSize()));
    ElementProperty->InitializeValue(TempElementStorage);

    for (int32 i = 0; i < SetArray.Num(); i++) {
      const TSharedPtr<FJsonValue> &Element = SetArray[i];
      DeserializePropertyValue(ElementProperty, Element.ToSharedRef(),
                               TempElementStorage);

      const int32 NewElementIndex =
          SetHelper.AddDefaultValue_Invalid_NeedsRehash();
      uint8 *NewElementPtr = SetHelper.GetElementPtr(NewElementIndex);

      /* Copy over imported key from temporary storage */
      ElementProperty->CopyCompleteValue_InContainer(NewElementPtr,
                                                     TempElementStorage);
    }
    SetHelper.Rehash();

    ElementProperty->DestroyValue(TempElementStorage);
    FMemory::Free(TempElementStorage);
  } else if (ArrayProperty) {
    FProperty *ElementProperty = ArrayProperty->Inner;
    FScriptArrayHelper ArrayHelper(ArrayProperty, OutValue);
    const TArray<TSharedPtr<FJsonValue>> &SetArray = NewJsonValue->AsArray();
    ArrayHelper.EmptyValues();

    for (int32 i = 0; i < SetArray.Num(); i++) {
      const TSharedPtr<FJsonValue> &Element = SetArray[i];
      const uint32 AddedIndex = ArrayHelper.AddValue();
      uint8 *ValuePtr = ArrayHelper.GetRawPtr(AddedIndex);
      DeserializePropertyValue(ElementProperty, Element.ToSharedRef(),
                               ValuePtr);
    }
  } else if (Property->IsA<FMulticastDelegateProperty>()) {
  } else if (Property->IsA<FDelegateProperty>()) {
  } else if (CastField<const FInterfaceProperty>(Property)) {
  } else if (FSoftObjectProperty *SoftObjectProperty =
                 CastField<FSoftObjectProperty>(Property)) {
    TSharedPtr<FJsonObject> SoftJsonObjectProperty;
    FString PathString = "";

    switch (NewJsonValue->Type) {
    /* UEParse, extract it from the object */
    case EJson::Object:
      SoftJsonObjectProperty = NewJsonValue->AsObject();
      PathString =
          SoftJsonObjectProperty->GetStringField(TEXT("AssetPathName"));
      break;

    /* Older game builds */
    default:
      PathString = NewJsonValue->AsString();
      break;
    }

    if (PathString != "") {
      FSoftObjectPtr *ObjectPtr = static_cast<FSoftObjectPtr *>(OutValue);
      *ObjectPtr = FSoftObjectPath(PathString);

      if (!ObjectPtr->LoadSynchronous()) {
        /* Try importing it using Cloud */
        FString PackagePath;
        FString AssetName;
        PathString.Split(".", &PackagePath, &AssetName);
        TObjectPtr<UObject> T;

        FString PropertyClassName =
            SoftObjectProperty->PropertyClass->GetName();

        IImporter::DownloadWrapper(T, PropertyClassName, AssetName,
                                   PackagePath);
      }
    }
  } else if (const FObjectPropertyBase *ObjectProperty =
                 CastField<const FObjectPropertyBase>(Property)) {
    /* Need to serialize full UObject for object property */
    TObjectPtr<UObject> Object = nullptr;

    if (NewJsonValue->IsNull()) {
      ObjectProperty->SetObjectPropertyValue(OutValue, nullptr);
    }

    if (NewJsonValue->Type == EJson::Object) {
      const TSharedPtr<FJsonObject> JsonValueAsObject = NewJsonValue->AsObject();
      if (!JsonValueAsObject.IsValid()) {
        return;
      }

      FString ObjectNameField;
      JsonValueAsObject->TryGetStringField(TEXT("ObjectName"), ObjectNameField);
      const bool IsParticleModule = ObjectNameField.Contains(TEXT(":ParticleModule"));

      if (!IsParticleModule) {
        if (Importer == nullptr) {
          Importer = new IImporter();
        }

        Importer->SetParent(ObjectSerializer ? ObjectSerializer->Parent : nullptr);
        Importer->LoadExport(&JsonValueAsObject, Object);

        if (Object != nullptr && IsValid(Object.Get())) {
          const bool bIsActorComponent =
              Object->IsA(UActorComponent::StaticClass());

          if (!bIsActorComponent) {
            ObjectProperty->SetObjectPropertyValue(OutValue, Object);
          } else {
            if (FUObjectExport *TargetExport =
                    ExportsContainer->GetExportByObjectPath(
                        JsonValueAsObject)) {
              FUObjectJsonValueExport Properties =
                  TargetExport->GetObject(TEXT("Properties"));

              if (TargetExport->Has(TEXT("LODData"))) {
                Properties.SetArray(TEXT("LODData"),
                                    TargetExport->GetArray(TEXT("LODData")));
              }

              if (ObjectProperty->NamePrivate != "AttachParent") {
                ObjectSerializer->DeserializeObjectProperties(
                    Properties.JsonObject, Object);
              } else {
                ObjectProperty->SetObjectPropertyValue(OutValue, Object);
              }
            }

            if (UStaticMeshComponent *StaticMeshComponent =
                    Cast<UStaticMeshComponent>(Object.Get())) {
              StaticMeshComponent->PostEditImport();
            }
          }
        }

        if (!ObjectProperty->GetObjectPropertyValue(OutValue) &&
            ObjectSerializer->bUseExperimentalSpawning) {
          if (FUObjectExport *TargetExport =
                  ExportsContainer->GetExportByObjectPath(JsonValueAsObject)) {
            FUObjectJsonValueExport Properties =
                TargetExport->GetObject(TEXT("Properties"));

            if (TargetExport->Has(TEXT("LODData"))) {
              Properties.SetArray(TEXT("LODData"),
                                  TargetExport->GetArray(TEXT("LODData")));
            }

            ObjectSerializer->SpawnExport(TargetExport);
          }
        }
      }

      FString ObjectName =
          JsonValueAsObject->GetStringField(TEXT("ObjectName"));
      FString ObjectPath =
          JsonValueAsObject->GetStringField(TEXT("ObjectPath"));
      FString ObjectOuter;
      int ObjectIndex = -1;

      if (ObjectName.Contains(".")) {
        ObjectName.Split(".", &ObjectOuter, &ObjectName);
        ObjectName.Split("'", &ObjectName, nullptr);
      }

      if (ObjectName.Contains(":")) {
        ObjectName.Split(":", nullptr, &ObjectName);
        ObjectName.Split("'", &ObjectName, nullptr);
      }

      if (ObjectPath.Contains(".")) {
        FString ObjectIndexString;
        ObjectPath.Split(".", nullptr, &ObjectIndexString);

        ObjectIndex = FCString::Atoi(*ObjectIndexString);
      }

      if (ExportsContainer) {
        if (FUObjectExport *Export = ExportsContainer->Find(ObjectName);
            Export && Export->Object != nullptr) {
          if (UObject *FoundObject = Export->Object) {
            if (IsValid(FoundObject)) {
              ObjectProperty->SetObjectPropertyValue(OutValue, FoundObject);
            }
          }
        }
      }

      if (ObjectName.Contains(".")) {
        TArray<FString> Parts;
        ObjectName.ParseIntoArray(Parts, TEXT("."), true);

        FString Penultimate =
            Parts.Num() > 1 ? Parts[Parts.Num() - 2] : TEXT("");
        FString LastSegment = Parts.Num() > 0 ? Parts.Last() : TEXT("");

        ObjectName = LastSegment;
        ObjectOuter = Penultimate;
      }

      if (!ObjectOuter.IsEmpty()) {
        if (ObjectOuter.Contains(":")) {
          ObjectOuter.Split(":", nullptr, &ObjectOuter);
        }

        if (FUObjectExport *Export =
                ExportsContainer->Find(ObjectName, ObjectOuter);
            Export && Export->Object != nullptr) {
          if (UObject *FoundObject = Export->Object) {
            if (IsValid(FoundObject)) {
              ObjectProperty->SetObjectPropertyValue(OutValue, FoundObject);
            }
          }
        }
      }

      if (FallbackToParentTrace) {
        if (UObject *Parent = ObjectSerializer ? ObjectSerializer->Parent
                                               : nullptr;
            IsValid(Parent)) {
          FString Name = Parent->GetName();

          if (FUObjectExport *Export = ExportsContainer->Find(ObjectName, Name);
              Export && Export->Object != nullptr) {
            if (UObject *FoundObject = Export->Object) {
              if (IsValid(FoundObject)) {
                ObjectProperty->SetObjectPropertyValue(OutValue, FoundObject);
              }
            }
          }
        }
      }

      if (ObjectIndex != -1 && ExportsContainer) {
        if (FUObjectExport *Export = ExportsContainer->FindByPositionAndName(
                ObjectIndex, ObjectName);
            Export && Export->Object != nullptr) {
          if (UObject *FoundObject = Export->Object) {
            if (IsValid(FoundObject)) {
              ObjectProperty->SetObjectPropertyValue(OutValue, FoundObject);
            }
          }
        }
      }

      /* Too extreme it seems */
#if 0
			FUObjectExport NewExport(JsonValueAsObject);

			const TArray<FName> TreeSegments = NewExport.GetOuterTreeSegments();

			if (TreeSegments.Num() > 0) {
				if (FUObjectExport& FoundExport = ExportsContainer->FindByTreeSegment(TreeSegments); FoundExport.Object != nullptr) {
					ObjectProperty->SetObjectPropertyValue(OutValue, FoundExport.Object);
				}
			}
#endif
    }
  } else if (const FStructProperty *StructProperty =
                 CastField<const FStructProperty>(Property)) {
    if (StructProperty->Struct == FGameplayTag::StaticStruct()) {
      FGameplayTag *GameplayTagStr = static_cast<FGameplayTag *>(OutValue);
      FGameplayTag NewTag = FGameplayTag::RequestGameplayTag(
          FName(*NewJsonValue->AsObject()->GetStringField(TEXT("TagName"))),
          false);
      *GameplayTagStr = NewTag;
      return;
    }

    /* FGameplayTagContainer (handled from UEParse data) */
    if (StructProperty->Struct == FGameplayTagContainer::StaticStruct()) {
      FGameplayTagContainer *GameplayTagContainerStr =
          static_cast<FGameplayTagContainer *>(OutValue);

      auto GameplayTags = JsonValue->AsArray();

      for (TSharedPtr GameplayTagValue : GameplayTags) {
        FString GameplayTagString = GameplayTagValue->AsString();
        FGameplayTag GameplayTag =
            FGameplayTag::RequestGameplayTag(FName(*GameplayTagString));

        GameplayTagContainerStr->AddTag(GameplayTag);
      }

      return;
    }

    if (StructProperty->Struct == FMovieSceneFrameRange::StaticStruct()) {
      FMovieSceneFrameRange *MovieSceneFrameRange =
          static_cast<FMovieSceneFrameRange *>(OutValue);
      TSharedPtr<FJsonObject> JsonObject =
          NewJsonValue->AsObject()->GetObjectField(TEXT("Value"));

      FMovieSceneFrameRange Range;

      if (JsonObject->HasField(TEXT("LowerBound"))) {
        TSharedPtr<FJsonObject> Bound =
            JsonObject->GetObjectField(TEXT("LowerBound"));
        TSharedPtr<FJsonObject> BoundValue =
            Bound->GetObjectField(TEXT("Value"));

        int32 Type = Bound->GetIntegerField(TEXT("Type"));
        int32 Value = BoundValue->GetIntegerField(TEXT("Value"));

        FFrameNumber BoundFrame;
        BoundFrame.Value = Value;

        if (Type == 0) {
          Range.Value.SetLowerBound(
              TRangeBound<FFrameNumber>::Exclusive(BoundFrame));
        } else if (Type == 1) {
          Range.Value.SetLowerBound(
              TRangeBound<FFrameNumber>::Inclusive(BoundFrame));
        }
      }

      if (JsonObject->HasField(TEXT("UpperBound"))) {
        TSharedPtr<FJsonObject> Bound =
            JsonObject->GetObjectField(TEXT("UpperBound"));
        TSharedPtr<FJsonObject> BoundValue =
            Bound->GetObjectField(TEXT("Value"));

        int32 Type = Bound->GetIntegerField(TEXT("Type"));
        int32 Value = BoundValue->GetIntegerField(TEXT("Value"));

        FFrameNumber BoundFrame;
        BoundFrame.Value = Value;

        if (Type == 0) {
          Range.Value.SetUpperBound(
              TRangeBound<FFrameNumber>::Exclusive(BoundFrame));
        } else if (Type == 1) {
          Range.Value.SetUpperBound(
              TRangeBound<FFrameNumber>::Inclusive(BoundFrame));
        }
      }

      *MovieSceneFrameRange = Range;

      return;
    }

    if (StructProperty->Struct->GetFName() == "SoftObjectPath") {
      TSharedPtr<FJsonObject> SoftJsonObjectProperty;
      FString PathString = "";

      SoftJsonObjectProperty = NewJsonValue->AsObject();
      PathString =
          SoftJsonObjectProperty->GetStringField(TEXT("AssetPathName"));

      if (PathString != "") {
        FSoftObjectPtr *ObjectPtr = static_cast<FSoftObjectPtr *>(OutValue);
        *ObjectPtr = FSoftObjectPath(PathString);

        if (!ObjectPtr->LoadSynchronous()) {
          /* Try importing it using Cloud */
          FString PackagePath;
          FString AssetName;
          PathString.Split(".", &PackagePath, &AssetName);
          TObjectPtr<UObject> T;

          FString PropertyClassName = "DataAsset";

          IImporter::DownloadWrapper(T, PropertyClassName, AssetName,
                                     PackagePath);
        }
      }
    }

    /* JSON for FGuids are FStrings */
    FString OutString;

    if (JsonValue->TryGetString(OutString)) {
      FGuid GUID = FGuid(OutString); /* Create GUID from String */

      TSharedRef<FJsonObject> SharedObject = MakeShareable(new FJsonObject());
      SharedObject->SetNumberField(TEXT("A"), GUID.A);
      SharedObject->SetNumberField(TEXT("B"), GUID.B);
      SharedObject->SetNumberField(TEXT("C"), GUID.C);
      SharedObject->SetNumberField(TEXT("D"), GUID.D);

      const TSharedRef<FJsonValue> NewValue =
          MakeShareable(new FJsonValueObject(SharedObject));
      NewJsonValue = NewValue;
    }

    if (StructProperty->Struct == FFontData::StaticStruct()) {
      FFontData *FontData = static_cast<FFontData *>(OutValue);
      TSharedPtr<FJsonObject> JsonObject = NewJsonValue->AsObject();

      if (JsonObject->HasField(TEXT("LocalFontFaceAsset"))) {
        TSharedPtr<FJsonObject> LocalFontFaceExport =
            JsonObject->GetObjectField(TEXT("LocalFontFaceAsset"));

        if (Importer == nullptr) {
          Importer = new IImporter();
        }

        TObjectPtr<UFontFace> FontFacePtr;

        Importer->SetParent(ObjectSerializer->Parent);
        Importer->LoadExport(&LocalFontFaceExport, FontFacePtr);

        if (UFontFace *FontFace = FontFacePtr.Get()) {
          *FontData = FFontData(FontFace, 0);
        }
      }
    }

    /* To serialize struct, we need its type and value pointer, because struct
     * value doesn't contain type information */
    DeserializeStruct(StructProperty->Struct,
                      NewJsonValue->AsObject().ToSharedRef(), OutValue);

#if ENGINE_UE4
    /* If we're importing from UE5 to UE4, adjust the material attribute nodes
     * to adjust for attributes that don't exist */
    if (Property->GetCPPType(nullptr, CPPF_None) == TEXT("FExpressionInput")) {
      if (GJsonAsAssetRuntime.IsUE5()) {
        FExpressionInput *ExpressionInput =
            static_cast<FExpressionInput *>(OutValue);

        if (ExpressionInput && ExpressionInput->OutputIndex > 10 &&
            ExpressionInput->Expression &&
            ExpressionInput->Expression->GetFName().ToString().Contains(
                "MaterialExpressionBreakMaterialAttributes")) {

          ExpressionInput->OutputIndex += 2;
        }
      }

      if (GJsonAsAssetRuntime.IsOlderUE4Target()) {
        FExpressionInput *ExpressionInput =
            static_cast<FExpressionInput *>(OutValue);

        if (ExpressionInput && ExpressionInput->Expression &&
            ExpressionInput->Expression->GetFName().ToString().Contains(
                "MaterialExpressionBreakMaterialAttributes")) {
          if (ExpressionInput->OutputIndex > 3) {
            ExpressionInput->OutputIndex += 1;
          }

          if (ExpressionInput->OutputIndex > 8) {
            ExpressionInput->OutputIndex += 1;
          }
        }
      }
    }
#endif

    /* If there's a missing distribution, create it from the lookup table */
    if (IsStructPropertyADistribution(StructProperty)) {
      if (FRawDistribution *RawDistribution =
              static_cast<FRawDistribution *>(OutValue)) {
        const bool IsFloat = IsFloatDistribution(StructProperty);

        if (!GetDistribution(RawDistribution, IsFloat)) {
          if (UDistribution *NewDistribution = DecookDistribution(
                  ObjectSerializer->Parent, *RawDistribution, IsFloat)) {
            SetDistribution(RawDistribution, NewDistribution, IsFloat);
          }
        }
      }
    }
  } else if (const FByteProperty *ByteProperty =
                 CastField<const FByteProperty>(Property)) {
    /* If we have a string provided, make sure Enum is not null */
    if (JsonValue->Type == EJson::String) {
      FString EnumAsString = JsonValue->AsString();

      /* Some byte properties are plain bytes and do not have enum metadata. */
      if (ByteProperty->Enum == nullptr) {
        int64 ParsedAsNumber = 0;
        if (!LexTryParseString(ParsedAsNumber, *EnumAsString)) {
          UE_LOG(LogJsonAsAsset, Warning,
                 TEXT("Byte property '%s' received non-numeric string '%s' "
                      "with no enum metadata; defaulting to 0."),
                 *Property->GetName(), *EnumAsString);
          ParsedAsNumber = 0;
        }

        ByteProperty->SetIntPropertyValue(OutValue, ParsedAsNumber);
      } else {
        int64 EnumerationValue =
            ByteProperty->Enum->GetValueByNameString(EnumAsString);
        if (EnumerationValue == INDEX_NONE &&
            EnumAsString.Contains(TEXT("::"))) {
          FString ScopedEnumAsString = EnumAsString;
          ScopedEnumAsString.Split(TEXT("::"), nullptr, &ScopedEnumAsString,
                                   ESearchCase::CaseSensitive,
                                   ESearchDir::FromEnd);
          EnumerationValue =
              ByteProperty->Enum->GetValueByNameString(ScopedEnumAsString);
        }

        /* Something's wrong, fallback to first entry */
        if (EnumerationValue == INDEX_NONE) {
          UE_LOG(LogJsonAsAsset, Warning,
                 TEXT("Invalid enum value for byte property '%s'!"),
                 *Property->GetName());
          UE_LOG(LogJsonAsAsset, Warning, TEXT("Enum name: %s"), *EnumAsString);
          UE_LOG(LogJsonAsAsset, Warning, TEXT("Enum type: %s"),
                 *ByteProperty->Enum->GetName());
          EnumerationValue = 0;
        }

        ByteProperty->SetIntPropertyValue(OutValue, EnumerationValue);
      }
    } else {
      /* Should be a number, set property value accordingly */
      const int64 NumberValue = static_cast<int64>(NewJsonValue->AsNumber());
      ByteProperty->SetIntPropertyValue(OutValue, NumberValue);
    }
    /* Primitives below, they are serialized as plain json values */
  } else if (const FNumericProperty *NumberProperty =
                 CastField<const FNumericProperty>(Property)) {
    const double NumberValue = NewJsonValue->AsNumber();
    if (NumberProperty->IsFloatingPoint()) {
      NumberProperty->SetFloatingPointPropertyValue(OutValue, NumberValue);
    }

    else {
      NumberProperty->SetIntPropertyValue(OutValue,
                                          static_cast<int64>(NumberValue));
    }
  } else if (const FBoolProperty *BoolProperty =
                 CastField<const FBoolProperty>(Property)) {
    const bool BooleanValue = NewJsonValue->AsBool();
    BoolProperty->SetPropertyValue(OutValue, BooleanValue);
  } else if (Property->IsA<FStrProperty>()) {
    const FString StringValue = NewJsonValue->AsString();
    *static_cast<FString *>(OutValue) = StringValue;
  } else if (const FEnumProperty *EnumProperty =
                 CastField<const FEnumProperty>(Property)) {
    FString EnumAsString = NewJsonValue->AsString();

    if (EnumAsString.Contains("::")) {
      EnumAsString.Split("::", nullptr, &EnumAsString);
    }

    /* Prefer readable enum names in result json to raw numbers */
    int64 EnumerationValue =
        EnumProperty->GetEnum()->GetValueByNameString(EnumAsString);

    if (EnumerationValue != INDEX_NONE) {
      EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(
          OutValue, EnumerationValue);
    }
  } else if (Property->IsA<FNameProperty>()) {
    /* Name is perfectly representable as string */
    const FString NameString = NewJsonValue->AsString();
    *static_cast<FName *>(OutValue) = *NameString;
  } else if (const FTextProperty *TextProperty =
                 CastField<const FTextProperty>(Property)) {
    const FString SerializedValue = NewJsonValue->AsString();

    if (!SerializedValue.IsEmpty()) {
      FTextStringHelper::ReadFromBuffer(*SerializedValue,
                                        *static_cast<FText *>(OutValue));
    } else {
      /* TODO: Somehow add other needed things like Namespace, Key, and
       * LocalizedString */
      TSharedPtr<FJsonObject> Object = NewJsonValue->AsObject().ToSharedRef();

      /* Retrieve properties */
      FString TextNamespace = Object->GetStringField(TEXT("Namespace"));
      FString UniqueKey = Object->GetStringField(TEXT("Key"));
      FString SourceString = Object->GetStringField(TEXT("SourceString"));
      FString StringTableId;
      Object->TryGetStringField(TEXT("TableId"), StringTableId);

      if (!StringTableId.IsEmpty()) {
        const FText TableText =
            FText::FromStringTable(FName(*StringTableId), UniqueKey);
        TextProperty->SetPropertyValue(OutValue, TableText);
      } else {
        TextProperty->SetPropertyValue(
            OutValue,
            FText::AsLocalizable_Advanced(*TextNamespace, *UniqueKey,
                                          *SourceString));
      }
    }
  } else if (CastField<const FFieldPathProperty>(Property)) {
    FFieldPath FieldPath;
    FieldPath.Generate(*NewJsonValue->AsString());
    *static_cast<FFieldPath *>(OutValue) = FieldPath;
  }
#if ENGINE_UE5
  else if (const FOptionalProperty *OptionalProperty =
               CastField<const FOptionalProperty>(Property)) {
    /* Null and explicit unset markers map to an unset optional value. */
    if (NewJsonValue->IsNull()) {
      OptionalProperty->MarkUnset(OutValue);
    } else if (NewJsonValue->Type == EJson::Object) {
      const TSharedPtr<FJsonObject> OptionalObject = NewJsonValue->AsObject();
      bool bIsSet = true;

      if (OptionalObject.IsValid() &&
          OptionalObject->TryGetBoolField(TEXT("IsSet"), bIsSet) && !bIsSet) {
        OptionalProperty->MarkUnset(OutValue);
      } else {
        TSharedRef<FJsonValue> OptionalValue = NewJsonValue;
        if (OptionalObject.IsValid() &&
            OptionalObject->HasField(TEXT("Value"))) {
          OptionalValue =
              OptionalObject->TryGetField(TEXT("Value")).ToSharedRef();
        }

        void *OptionalData =
            OptionalProperty->MarkSetAndGetInitializedValuePointerToReplace(
                OutValue);
        DeserializePropertyValue(OptionalProperty->GetValueProperty(),
                                 OptionalValue, OptionalData);
      }
    } else {
      void *OptionalData =
          OptionalProperty->MarkSetAndGetInitializedValuePointerToReplace(
              OutValue);
      DeserializePropertyValue(OptionalProperty->GetValueProperty(),
                               NewJsonValue, OptionalData);
    }
  }
#endif
  else {
    UE_LOG(LogJsonAsAssetPropertySerializer, Warning,
           TEXT("Found unsupported property type when deserializing value: %s "
                "(property: %s); skipping."),
           *Property->GetClass()->GetName(), *Property->GetName());
  }
}

void UPropertySerializer::DisablePropertySerialization(
    const UStruct *Struct, const FName PropertyName) {
  FProperty *Property = Struct->FindPropertyByName(PropertyName);
  checkf(Property, TEXT("Cannot find Property %s in Struct %s"),
         *PropertyName.ToString(), *Struct->GetPathName());
  BlacklistedProperties.Add(Property);
}

void UPropertySerializer::AddStructSerializer(
    UScriptStruct *Struct, const TSharedPtr<FStructSerializer> &Serializer) {
  StructSerializers.Add(Struct, Serializer);
}

bool UPropertySerializer::ShouldDeserializeProperty(FProperty *Property) const {
  /* Skip deprecated properties */
  if (Property->HasAnyPropertyFlags(CPF_Deprecated)) {
    return false;
  }

  /* Skip blacklisted properties */
  if (this != nullptr && this && BlacklistedProperties.IsValidIndex(0) &&
      BlacklistedProperties.Contains(Property)) {
    return false;
  }

  return true;
}

void UPropertySerializer::DeserializeStruct(
    UScriptStruct *Struct, const TSharedRef<FJsonObject> &Properties,
    void *OutValue) const {
  FStructSerializer *StructSerializer = GetStructSerializer(Struct);
  StructSerializer->Deserialize(Struct, OutValue, Properties);
}

FStructSerializer *
UPropertySerializer::GetStructSerializer(const UScriptStruct *Struct) const {
  check(Struct);
  TSharedPtr<FStructSerializer> const *StructSerializer =
      StructSerializers.Find(Struct);
  return StructSerializer && ensure(StructSerializer->IsValid())
             ? StructSerializer->Get()
             : FallbackStructSerializer.Get();
}

UE_ENABLE_OPTIMIZATION
