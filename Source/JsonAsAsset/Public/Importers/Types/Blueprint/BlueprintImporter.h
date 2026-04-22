/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IBlueprintImporter final : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	
	virtual bool Import() override;

	void SetupConstructionScript() const;
	bool SetupInheritableComponentHandler() const;
	bool SetupImplementedInterfaces() const;
	void SetupTimelineComponents() const;
	bool ShouldCompileImmediately() const;

protected:
	UBlueprint* Blueprint = nullptr;
};

REGISTER_IMPORTER(IBlueprintImporter, (TArray<FString>{ 
	TEXT("BlueprintGeneratedClass")
}), TEXT("Blueprint Assets"));
