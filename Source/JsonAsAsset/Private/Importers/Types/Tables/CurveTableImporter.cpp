/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Tables/CurveTableImporter.h"
#include "Dom/JsonObject.h"

class UCurveTableAccessor final : public UCurveTable {
public:
  void ChangeTableMode(const ECurveTableMode Mode) { CurveTableMode = Mode; }
};

UObject *ICurveTableImporter::CreateAsset(UObject *CreatedAsset) {
  return IImporter::CreateAsset(NewObject<UCurveTableAccessor>(
      GetPackage(), UCurveTableAccessor::StaticClass(), *GetAssetName(),
      RF_Public | RF_Standalone));
}

bool ICurveTableImporter::Import() {
  UCurveTable *CurveTable = Create<UCurveTable>();
  if (!CurveTable || !GetAssetData().IsValid() ||
      !GetAssetData()->HasField(TEXT("Rows"))) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("CurveTable import failed for '%s': missing Rows."),
           *GetAssetName());
    return false;
  }

  const TSharedPtr<FJsonObject> RowData =
      GetAssetData()->GetObjectField(TEXT("Rows"));
  if (!RowData.IsValid()) {
    UE_LOG(LogJsonAsAsset, Error,
           TEXT("CurveTable import failed for '%s': invalid Rows object."),
           *GetAssetName());
    return false;
  }

  /* Used to determine curve type */
  ECurveTableMode CurveTableMode = ECurveTableMode::RichCurves;
  {
    if (FString CurveMode;
        GetAssetData()->TryGetStringField(TEXT("CurveTableMode"), CurveMode))
      CurveTableMode = static_cast<ECurveTableMode>(
          StaticEnum<ECurveTableMode>()->GetValueByNameString(CurveMode));

    UCurveTableAccessor *CurveTableAccessor =
        Cast<UCurveTableAccessor>(CurveTable);
    if (!CurveTableAccessor) {
      UE_LOG(LogJsonAsAsset, Error,
             TEXT("CurveTable import failed for '%s': created asset is not the "
                  "accessor subclass."),
             *GetAssetName());
      return false;
    }

    CurveTableAccessor->ChangeTableMode(CurveTableMode);
  }

  /* Loop throughout row data, and deserialize */
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : RowData->Values) {
    if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object ||
        !Pair.Value->AsObject().IsValid()) {
      UE_LOG(LogJsonAsAsset, Warning,
             TEXT("CurveTable '%s': skipping malformed row '%s'."),
             *GetAssetName(), *Pair.Key);
      continue;
    }

    const TSharedPtr<FJsonObject> CurveData = Pair.Value->AsObject();

    if (CurveTableMode == ECurveTableMode::RichCurves) {
      FRichCurve &Curve = CurveTable->AddRichCurve(FName(*Pair.Key));

      GetPropertySerializer()->DeserializeStruct(
          Curve.StaticStruct(), CurveData.ToSharedRef(), &Curve);
    } else {
      FSimpleCurve &Curve = CurveTable->AddSimpleCurve(FName(*Pair.Key));

      GetPropertySerializer()->DeserializeStruct(
          Curve.StaticStruct(), CurveData.ToSharedRef(), &Curve);
    }

    /* Update Curve Table */
    CurveTable->OnCurveTableChanged().Broadcast();
    CurveTable->Modify(true);
  }

  /* Handle edit changes, and add it to the content browser */
  return OnAssetCreation(CurveTable);
}