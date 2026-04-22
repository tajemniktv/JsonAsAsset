/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Importers/Types/Audio/SoundWaveImporter.h"

#include "Sound/SoundWave.h"

UObject* ISoundWaveImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<USoundWave>(GetPackage(), GetAssetClass(), FName(GetAssetName()), RF_Public | RF_Standalone));
}

bool ISoundWaveImporter::Import() {
	USoundWave* SoundWave = Create<USoundWave>();
	if (!SoundWave) {
		return false;
	}

	SoundWave->MarkPackageDirty();

	DeserializeExports(SoundWave);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), SoundWave);

	return OnAssetCreation(SoundWave);
}

