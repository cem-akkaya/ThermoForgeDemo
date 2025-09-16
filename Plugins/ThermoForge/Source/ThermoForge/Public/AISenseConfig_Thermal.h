#pragma once

#include "CoreMinimal.h"
#include "Perception/AISenseConfig.h"
#include "AISenseConfig_Thermal.generated.h"

class UAISense_Thermal;

/**
 * Designer-facing config for Thermal sense (appears on AIPerceptionComponent).
 * Runtime module; depends on AIModule.
 */
UCLASS(EditInlineNew, Config=Game, DefaultConfig, meta=(DisplayName="AI Thermal sense config"))
class THERMOFORGE_API UAISenseConfig_Thermal : public UAISenseConfig
{
    GENERATED_BODY()

public:
    UAISenseConfig_Thermal();

    /** Max sensing range (cm). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Ranges", meta=(ClampMin="0"))
    float MaxRange = 4000.f; // 2500 cm = 25 m

    /** Near/Mid thresholds (cm) for LOD update rates. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Ranges", meta=(ClampMin="0"))
    float NearRange = 2000.f;   // 6 m

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Ranges", meta=(ClampMin="0"))
    float MidRange  = 3000.f;  // 14 m

    /** Degrees C above local ambient required to consider “interesting”. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Detection")
    float HeatThresholdC = 4.0f;

    /** Optional directional cone (else spherical). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Shape")
    bool bDirectional = false;

    /** Half-angle (deg) if directional. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Shape", meta=(EditCondition="bDirectional", ClampMin="0.0", ClampMax="180.0"))
    float FOV = 60.f;

    /** Use visibility/permeability checks when sensing through obstacles. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Occlusion")
    bool bUseLineOfSight = true;

    /** Steps along the LoS ray if doing multi-sample/permeability. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Occlusion", meta=(EditCondition="bUseLineOfSight", ClampMin="1", ClampMax="16"))
    int32 LoSSteps = 8;

    /** Update cadence per LOD ring (seconds). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Update", meta=(ClampMin="0.01"))
    float NearUpdate = 0.10f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Update", meta=(ClampMin="0.01"))
    float MidUpdate  = 0.33f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Update", meta=(ClampMin="0.01"))
    float FarUpdate  = 1.00f;

    /** Optional tag filter to only consider actors with this tag as “emitters”. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category="Thermal|Emitters")
    FName EmitterActorTag;

    /** 5.6: only override the virtual. Do NOT assign Implementation in ctor. */
    virtual TSubclassOf<UAISense> GetSenseImplementation() const override;
};
