/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IStaticMeshImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(IStaticMeshImporter, {
	TEXT("StaticMesh")
}, "Mesh Assets");

