/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Tables/DataTableImporter.h"

UObject *IDataTableImporter::CreateAsset(UObject *CreatedAsset) {
  return IImporter::CreateAsset(
      NewObject<UDataTable>(GetPackage(), UDataTable::StaticClass(),
                            *GetAssetName(), RF_Public | RF_Standalone));
}

bool IDataTableImporter::Import() {
  UDataTable *DataTable = Create<UDataTable>();
  if (!DataTable || !GetAssetData().IsValid() ||
      !GetAssetData()->HasField(TEXT("RowStruct")) ||
      !GetAssetData()->HasField(TEXT("Rows"))) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("DataTable import failed for '%s': missing RowStruct or Rows."),
           *GetAssetName());
    return false;
  }

  /* ScriptClass for the Data Table */
  FString TableStruct;
  {
    /* --- Properties --> RowStruct --> ObjectName */
    /* --- Class'StructClass' --> StructClass */
    const TSharedPtr<FJsonObject> RowStructObject =
        GetAssetData()->GetObjectField(TEXT("RowStruct"));
    if (!RowStructObject.IsValid() ||
        !RowStructObject->HasField(TEXT("ObjectName"))) {
      UE_LOG(
          LogJsonAsAsset, Error,
          TEXT("DataTable import failed for '%s': invalid RowStruct object."),
          *GetAssetName());
      return false;
    }

    RowStructObject->GetStringField(TEXT("ObjectName"))
        .Split("'", nullptr, &TableStruct);
    TableStruct.Split("'", &TableStruct, nullptr);
  }

  /* Find Table Row Struct */
#if UE5_6_BEYOND
  UScriptStruct *TableRowStruct = FindFirstObject<UScriptStruct>(*TableStruct);
  {
#else
  UScriptStruct *TableRowStruct =
      FindObject<UScriptStruct>(ANY_PACKAGE, *TableStruct);
  {
#endif
    if (TableRowStruct == nullptr) {
      AppendNotification(
          FText::FromString("DataTable Struct Missing: " + TableStruct),
          FText::FromString("The parent DataTable's struct definition could "
                            "not be found. Ensure the correct struct is "
                            "defined and referenced by this table."),
          2.0f, SNotificationItem::CS_Fail, true, 350.0f);

      return false;
    }

    DataTable->RowStruct = TableRowStruct;
  }

  /* Access Property Serializer */
  const UPropertySerializer *ObjectPropertySerializer =
      GetObjectSerializer()->GetPropertySerializer();
  const TSharedPtr<FJsonObject> RowData =
      GetAssetData()->GetObjectField(TEXT("Rows"));
  if (!RowData.IsValid()) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("DataTable import failed for '%s': Rows object is invalid."),
           *GetAssetName());
    return false;
  }

  /* Loop throughout row data, and deserialize */
  for (TPair<FString, TSharedPtr<FJsonValue>> &Pair : RowData->Values) {
    if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object ||
        !Pair.Value->AsObject().IsValid()) {
      UE_LOG(LogJsonAsAsset, Warning,
             TEXT("DataTable '%s': skipping malformed row '%s'."),
             *GetAssetName(), *Pair.Key);
      continue;
    }

    const TSharedPtr<FStructOnScope> ScopedStruct =
        MakeShareable(new FStructOnScope(TableRowStruct));
    TSharedPtr<FJsonObject> StructData = Pair.Value->AsObject();

    /* Deserialize, add row */
    ObjectPropertySerializer->DeserializeStruct(
        TableRowStruct, StructData.ToSharedRef(),
        ScopedStruct->GetStructMemory());
    DataTable->AddRow(*Pair.Key, *reinterpret_cast<const FTableRowBase *>(
                                     ScopedStruct->GetStructMemory()));
  }

  /* Handle edit changes, and add it to the content browser */
  return OnAssetCreation(DataTable);
}
