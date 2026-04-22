/* Copyright JsonAsAsset Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class UPanelWidget;
class UWidgetBlueprint;

class IWidgetBlueprintImporter final : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	virtual bool Import() override;

private:
	void HandlePanelSlots(UWidgetBlueprint* WidgetBlueprint, FUObjectExport* PanelExport, UPanelWidget* Panel);
};

REGISTER_IMPORTER(IWidgetBlueprintImporter, (TArray<FString>{
	TEXT("WidgetBlueprintGeneratedClass")
}), TEXT("Blueprint Assets"));
