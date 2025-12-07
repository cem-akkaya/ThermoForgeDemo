#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ThermoForgeSubsystem.generated.h"

class UThermoForgeSourceComponent;
class AThermoForgeVolume;
class UThermoForgeFieldAsset;
class UThermoForgeProjectSettings;

USTRUCT()
struct FThermoProbe
{
    GENERATED_BODY()
    FVector PosWS = FVector::ZeroVector;
    float   TempC = 0.f;
};

// ---------- RESULT STRUCT ----------
USTRUCT(BlueprintType)
struct FThermoForgeGridHit
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    bool bFound = false;

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    TObjectPtr<AThermoForgeVolume> Volume = nullptr;

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    FIntVector GridIndex = FIntVector::ZeroValue;

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    int32 LinearIndex = -1;

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    FVector CellCenterWS = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    double DistanceSq = TNumericLimits<double>::Max();

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    float CellSizeCm = 0.f;

    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    FDateTime QueryTimeUTC = FDateTime(0);

    /** Composed current temperature at that cell (°C). */
    UPROPERTY(BlueprintReadOnly, Category="ThermoForge")
    float CurrentTempC = 0.f;
};

// ---------- CLIMATE TYPE ----------
UENUM(BlueprintType)
enum class EThermoClimateType : uint8
{
    Arctic     UMETA(DisplayName="Arctic"),
    Cold       UMETA(DisplayName="Cold"),
    Temperate  UMETA(DisplayName="Temperate"),
    Warm       UMETA(DisplayName="Warm"),
    Desert     UMETA(DisplayName="Desert"),
    Tropical   UMETA(DisplayName="Tropical")
};

// ---------- CLIMATE STRUCT ----------
USTRUCT(BlueprintType)
struct FThermoClimateInfo
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Climate")
    float MeanAnnualTemperature = 10.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Climate")
    float AnnualTemperatureRange = 20.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Climate")
    EThermoClimateType ClimateType = EThermoClimateType::Temperate;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FThermoBakeProgress, float /*Progress01*/);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FThermoSourcesChanged);

UCLASS()
class THERMOFORGE_API UThermoForgeSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    // lifecycle
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // sources
    void RegisterSource(UThermoForgeSourceComponent* Source);
    void UnregisterSource(UThermoForgeSourceComponent* Source);
    void MarkSourceDirty(UThermoForgeSourceComponent* Source);
    int32 GetSourceCount() const;
    void StartNextBake();
    void GetAllSources(TArray<UThermoForgeSourceComponent*>& OutSources) const;

    UPROPERTY(BlueprintAssignable, Category="Thermo Forge")
    FThermoSourcesChanged OnSourcesChanged;

    // --------- Geometry-only bake ----------
    UFUNCTION(BlueprintCallable, Category="Thermo Forge")
    void KickstartSamplingFromVolumes();

    /** Occlusion between two points (0..1, 1=open) using physmat density + Beer–Lambert. */
    float OcclusionBetween(const FVector& A, const FVector& B, float CellSizeCm) const;

    // --------- Queries / Composition ----------
    /** Compose current temperature (°C) at world position using baked geometry + runtime climate + dynamic sources. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="Thermo Forge|Query")
    float ComputeCurrentTemperatureAt(const FVector& WorldPos, bool bWinter, float TimeHours, float WeatherAlpha01) const;

    /** Find nearest baked grid point; also fills CurrentTempC using default preview knobs. */
    UFUNCTION(BlueprintCallable, Category="ThermoForge|Query")
    FThermoForgeGridHit QueryNearestBakedGridPoint(const FVector& WorldLocation, const FDateTime& QueryTimeUTC) const;

    /** Find nearest baked grid point with current UTC of player or system*/
    UFUNCTION(BlueprintCallable, Category="ThermoForge|Query")
    FThermoForgeGridHit QueryNearestBakedGridPointNow(const FVector& WorldLocation) const;
    
    /** Return overall climate type at given world position for annual. */
    UFUNCTION(BlueprintCallable, Category="ThermoForge|Climate")
    EThermoClimateType GetClimateTypeAtPoint(const FVector& WorldPos) const;

    // Compose temperature from *baked field only* (Ambient + Solar*SkyView).
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="ThermoForge|Query")
    float ComputeBakedOnlyTemperatureAt(const FVector& WorldPos, bool bWinter, float TimeHours, float WeatherAlpha01) const;

    // Find hottest/coldest baked cell near a point within RadiusCm (searches the right volume).
    UFUNCTION(BlueprintCallable, Category="ThermoForge|Query")
    bool FindBakedExtremeNear(const FVector& CenterWS, float RadiusCm, bool bHottest, FThermoForgeGridHit& OutHit, const FDateTime& QueryTimeUTC) const;

    // Progress

    void TickBake();
    TWeakObjectPtr<AThermoForgeVolume> BakeVolume;
    FIntVector BakeDim;
    int32 BakeTotalCells = 0;
    int32 BakeProcessed  = 0;

    float BakeCell = 0.f;
    FTransform BakeFrame;
    int32 Bake_ix0, Bake_iy0, Bake_iz0;

    TArray<float> BakeSky, BakeWall, BakeIndoor;
    TArray<FVector> BakeHemiDirs;

    FTimerHandle BakeTimerHandle;

    FThermoBakeProgress OnBakeProgress;

    // Access Settings over subsystem also
    const UThermoForgeProjectSettings* GetSettings() const;

    TArray<TWeakObjectPtr<AThermoForgeVolume>> BakeQueue;

    // Blue Noise Splat
    UPROPERTY(EditAnywhere, Category="ThermoForge|Probes")
    int32 MaxProbes = 1024;

    UPROPERTY(EditAnywhere, Category="ThermoForge|Probes", meta=(ClampMin="100", ClampMax="50000"))
    float ProbeRadiusCm = 400.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ThermoForge|Probes")
    float KernelRadiusCm = 150.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ThermoForge|Probes")
    TObjectPtr<UTexture2D> ProbesTex = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ThermoForge|Probes")
    TArray<FVector> ProbeOffsetsLS; // relative to CenterWS (in cm)

    TArray<FFloat16Color> ProbePixels;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ThermoForge|Probes")
    int32 NumProbes = 0;

    UFUNCTION(BlueprintCallable, Category="ThermoForge|Probes")
    void UpdateThermalProbesAndUpload(const FVector& CenterWS, bool bRegeneratePoints = false);


private:
    // helpers
    float TraceAmbientRay01(const FVector& P, const FVector& Dir, float MaxLen) const;

    static void TF_DumpFieldToSavedFolder(const FString& VolName,
        const FIntVector& Dim, float Cell, const FVector& OriginWS,
        const TArray<float>& SkyView01, const TArray<float>& WallPerm01, const TArray<float>& Indoor01);

    bool ComputeNearestInVolume(const AThermoForgeVolume* Vol, const FVector& WorldLocation, FThermoForgeGridHit& OutHit) const;
    bool VolumeContainsPoint(const AThermoForgeVolume* Vol, const FVector& WorldLocation) const;
    
    EThermoClimateType ClassifyClimateFromTemperature(float TempC) const;

#if WITH_EDITOR
    UThermoForgeFieldAsset* CreateAndSaveFieldAsset(AThermoForgeVolume* Volume, const FIntVector& Dim, float Cell, const FVector& FieldOriginWS, const FRotator& GridRotation,
                                                    const TArray<float>& SkyView01, const TArray<float>& WallPerm01, const TArray<float>& Indoor01) const;
#endif

    void CompactSources();

    // data
    TSet<TWeakObjectPtr<UThermoForgeSourceComponent>> SourceSet;

    FVector BakeFieldOriginWS;
};
