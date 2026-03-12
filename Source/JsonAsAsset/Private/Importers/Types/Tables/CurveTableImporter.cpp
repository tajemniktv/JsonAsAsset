/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Tables/CurveTableImporter.h"
#include "Dom/JsonObject.h"

#if !UE4_18_BELOW
class UCurveTableAccessor final : public UCurveTable {
public:
	void ChangeTableMode(const ECurveTableMode Mode) {
		CurveTableMode = Mode;
	}
};
#endif

UObject* ICurveTableImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UCurveTable>(GetPackage(), UCurveTable::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool ICurveTableImporter::Import() {
	const TSharedPtr<FJsonObject> RowData = GetAssetData()->GetObjectField(TEXT("Rows"));
	UCurveTable* CurveTable = Create<UCurveTable>();

#if !UE4_18_BELOW
	/* Used to determine curve type */
	ECurveTableMode CurveTableMode = ECurveTableMode::RichCurves; {
		if (FString CurveMode; GetAssetData()->TryGetStringField(TEXT("CurveTableMode"), CurveMode))
			CurveTableMode = static_cast<ECurveTableMode>(StaticEnum<ECurveTableMode>()->GetValueByNameString(CurveMode));

		UCurveTableAccessor* CurveTableAccessor = Cast<UCurveTableAccessor>(CurveTable);
		CurveTableAccessor->ChangeTableMode(CurveTableMode);
	}
#endif

	/* Loop throughout row data, and deserialize */
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RowData->Values) {
		const TSharedPtr<FJsonObject> CurveData = Pair.Value->AsObject();

#if !UE4_18_BELOW
		if (CurveTableMode == ECurveTableMode::RichCurves) {

			FRichCurve& Curve = CurveTable->AddRichCurve(FName(*Pair.Key));
			GetPropertySerializer()->DeserializeStruct(Curve.StaticStruct(), CurveData.ToSharedRef(), &Curve);
		} else {
			FSimpleCurve& Curve = CurveTable->AddSimpleCurve(FName(*Pair.Key));
			GetPropertySerializer()->DeserializeStruct(Curve.StaticStruct(), CurveData.ToSharedRef(), &Curve);

		}
#endif

		/* Update Curve Table */
#if !UE4_18_BELOW
		CurveTable->OnCurveTableChanged().Broadcast();
#endif
		CurveTable->Modify(true);
	}

	/* Handle edit changes, and add it to the content browser */
	return OnAssetCreation(CurveTable);
}