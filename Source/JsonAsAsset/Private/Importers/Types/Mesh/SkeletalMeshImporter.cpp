/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Mesh/SkeletalMeshImporter.h"

#include "Engine/SkeletalMesh.h"

UObject* ISkeletalMeshImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<USkeletalMesh>(GetPackage(), GetAssetClass(), FName(GetAssetName()), RF_Public | RF_Standalone));
}

bool ISkeletalMeshImporter::Import() {
	USkeletalMesh* SkeletalMesh = Create<USkeletalMesh>();
	if (!SkeletalMesh) {
		return false;
	}

	SkeletalMesh->MarkPackageDirty();

	DeserializeExports(SkeletalMesh);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), SkeletalMesh);

	return OnAssetCreation(SkeletalMesh);
}

