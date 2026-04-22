/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/UI/FontFaceImporter.h"

#include "Engine/FontFace.h"

UObject* IFontFaceImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UFontFace>(GetPackage(), GetAssetClass(), FName(GetAssetName()), RF_Public | RF_Standalone));
}

bool IFontFaceImporter::Import() {
	UFontFace* FontFace = Create<UFontFace>();
	if (!FontFace) {
		return false;
	}

	FontFace->MarkPackageDirty();

	DeserializeExports(FontFace);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), FontFace);

	return OnAssetCreation(FontFace);
}

