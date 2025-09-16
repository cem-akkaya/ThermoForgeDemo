#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"


class FThermoForgeEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /**  State for progress popup */
    TSharedPtr<SWindow> BakeProgressWindow;
    TSharedPtr<class SProgressBar> BakeProgressBar;
    
    void ShowBakeProgressPopup();
    void UpdateBakeProgress(float InProgress);
    void HideBakeProgressPopup();

private:

    /** Function to spawn the tab */
    TSharedRef<SDockTab> OnSpawnThermoForgeTab(const FSpawnTabArgs& SpawnTabArgs);

    /** Function to open the tab */
    void OpenThermoForgeTab();

    /** Register menus */
    void RegisterMenus();

    /** Buttons */
    FReply OnSpawnThermalVolumeClicked();
    void   SpawnThermalVolume();

    // Callbacks:
    FReply OnAddHeatSourceClicked();
    FReply OnKickstartSamplingClicked();
    FReply OnShowAllPreviewsClicked();
    FReply OnOpenSettingsClicked();
    FReply OnHideAllPreviewsClicked();
    FReply OnSetMeshInsulatedClicked();
    TSharedRef<SWidget> MakeToolButton(const FString& Label, const FName& Icon, FOnClicked OnClicked);
    
};