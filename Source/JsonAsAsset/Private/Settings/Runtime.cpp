/* Copyright JsonAsAsset Contributors 2024-2026 */

#include "Settings/Runtime.h"

#include "Misc/FileHelper.h"
#include "Utilities/EngineUtilities.h"

/* Define Global Struct */
FJRuntime GJsonAsAssetRuntime;

bool FJRuntime::IsOlderUE4Target() const {
	return MajorVersion == 4 && MinorVersion != -1 && MinorVersion < 14;
}

bool FJRuntime::IsUE5() const {
	return MajorVersion == 5;
}

void FJRuntime::Update() {
#if UE4_18_BELOW
	TCHAR AppDataBuffer[4096];
	FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"), AppDataBuffer, ARRAY_COUNT(AppDataBuffer));
	FString AppDataPath(AppDataBuffer);
#else
	FString AppDataPath = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
#endif
	AppDataPath = FPaths::Combine(AppDataPath, TEXT("FModel/AppSettings.json"));

#if UE4_18_BELOW
	FString FileContent;
	if (FFileHelper::LoadFileToString(FileContent, *AppDataPath)) {
		TSharedPtr<FJsonObject> JsonObject;
		if (DeserializeJSONObject(FileContent, JsonObject)) {
#else
	if (FString FileContent; FFileHelper::LoadFileToString(FileContent, *AppDataPath)) {
		if (TSharedPtr<FJsonObject> JsonObject; DeserializeJSONObject(FileContent, JsonObject)) {
#endif
			ExportDirectory.Path = JsonObject->GetStringField(TEXT("PropertiesDirectory")).Replace(TEXT("\\"), TEXT("/"));
		}
	}
}
