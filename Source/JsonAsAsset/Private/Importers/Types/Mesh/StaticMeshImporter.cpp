/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Mesh/StaticMeshImporter.h"

#include "Engine/StaticMesh.h"

UObject* IStaticMeshImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UStaticMesh>(GetPackage(), GetAssetClass(), FName(GetAssetName()), RF_Public | RF_Standalone));
}

bool IStaticMeshImporter::Import() {
	UStaticMesh* StaticMesh = Create<UStaticMesh>();
	if (!StaticMesh) {
		return false;
	}

	StaticMesh->MarkPackageDirty();

	DeserializeExports(StaticMesh);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), StaticMesh);

	return OnAssetCreation(StaticMesh);
}

