/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Curves/CurveLinearColorImporter.h"
#include "Curves/CurveLinearColor.h"
#include "Factories/CurveFactory.h"
#include "Utilities/JsonUtilities.h"

UObject *ICurveLinearColorImporter::CreateAsset(UObject *CreatedAsset) {
  UCurveLinearColorFactory *CurveFactory =
      NewObject<UCurveLinearColorFactory>();
  UCurveLinearColor *CurveLinearColor =
      Cast<UCurveLinearColor>(CurveFactory->FactoryCreateNew(
          UCurveLinearColor::StaticClass(), GetPackage(), *GetAssetName(),
          RF_Standalone | RF_Public, nullptr, GWarn));

  return IImporter::CreateAsset(CurveLinearColor);
}

bool ICurveLinearColorImporter::Import() {
  /* Array of containers */
  if (!GetAssetData().IsValid() ||
      !GetAssetData()->HasField(TEXT("FloatCurves"))) {
    UE_LOG(
        LogJsonAsAsset, Error,
        TEXT("CurveLinearColor import failed for '%s': missing FloatCurves."),
        *GetAssetName());
    return false;
  }

  TArray<TSharedPtr<FJsonValue>> FloatCurves =
      GetAssetData()->GetArrayField(TEXT("FloatCurves"));

  UCurveLinearColor *CurveLinearColor = Create<UCurveLinearColor>();
  if (!CurveLinearColor) {
    return false;
  }

  /* For each container, get keys */
  for (int i = 0; i < FloatCurves.Num(); i++) {
    if (!FloatCurves[i].IsValid() || FloatCurves[i]->Type != EJson::Object ||
        !FloatCurves[i]->AsObject().IsValid()) {
      continue;
    }

    if (i >= UE_ARRAY_COUNT(CurveLinearColor->FloatCurves)) {
      UE_LOG(LogJsonAsAsset, Warning,
             TEXT("CurveLinearColor '%s': channel index %d out of range; skipping."),
             *GetAssetName(), i);
      continue;
    }

    TArray<TSharedPtr<FJsonValue>> Keys =
        FloatCurves[i]->AsObject()->GetArrayField(TEXT("Keys"));

    CurveLinearColor->FloatCurves[i].Keys.Empty();

    /* Add keys to the array */
    for (int j = 0; j < Keys.Num(); j++) {
      if (!Keys[j].IsValid() || Keys[j]->Type != EJson::Object ||
          !Keys[j]->AsObject().IsValid()) {
        continue;
      }

      CurveLinearColor->FloatCurves[i].Keys.Add(
          ObjectToRichCurveKey(Keys[j]->AsObject()));
    }
  }

  /* Handle edit changes, and add it to the content browser */
  return OnAssetCreation(CurveLinearColor);
}
