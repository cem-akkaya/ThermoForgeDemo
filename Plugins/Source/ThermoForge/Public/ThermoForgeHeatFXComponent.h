#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ThermoForgeHeatFXComponent.generated.h"

class UPrimitiveComponent;
class UThermoForgeSourceComponent;

/** How to choose the “origin” the material reacts to */
UENUM(BlueprintType)
enum class EThermoOriginMode : uint8
{
	NearestSourceActor   UMETA(DisplayName="Nearest Source Actor (UThermoForgeSourceComponent)"),
	HottestPoint         UMETA(DisplayName="Hottest Nearby Point (probe ring)"),
	ColdestPoint         UMETA(DisplayName="Coldest Nearby Point (probe ring)")
};

/** Raised whenever the component updates heat data (after an actual change). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_SevenParams(
	FOnHeatUpdated,
	UThermoForgeHeatFXComponent*, Comp,
	float,  TemperatureC,
	float,  HeatStrength,
	FVector, HeatDirWS,
	float,  DistanceCm,
	FVector, SourcePosWS,
	EThermoOriginMode, OriginMode
);

/** Raised only when |ΔTemperature| >= ChangeThresholdC (a “big jump”). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_EightParams(
	FOnHeatJump,
	UThermoForgeHeatFXComponent*, Comp,
	float,  NewTemperatureC,
	float,  PrevTemperatureC,
	float,  DeltaC,
	float,  HeatStrength,
	FVector, HeatDirWS,
	float,  DistanceCm,
	FVector, SourcePosWS
);

/**
 * Minimal Thermo Forge FX bridge:
 * - Samples temperature at owner.
 * - Resolves an origin (nearest source OR hottest/coldest probe).
 * - Computes direction (owner → origin), intensity proxy, and distance.
 * - Emits events on significant changes.
 * - Writes Custom Primitive Data (CPD) for materials (dir/strength/source/intensity/radius).
 *
 * CPD layout (BaseIndex = CPDBaseIndex):
 *   [0..2] : HeatDirWS (XYZ)
 *   [3]    : HeatStrength (scalar)
 *   [4..6] : SourcePosWS (XYZ)
 *   [7]    : DistanceIntensity (authoring knob for material)
 *   [8]    : ReferenceRadiusCm  (authoring knob for material)
 */
UCLASS(ClassGroup=(ThermoForge), meta=(BlueprintSpawnableComponent))
class THERMOFORGE_API UThermoForgeHeatFXComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UThermoForgeHeatFXComponent();

	/** Fires whenever the internally cached heat payload changes. */
	UPROPERTY(BlueprintAssignable, Category="ThermoForge|Events")
	FOnHeatUpdated OnHeatUpdated;

	/** Fires only when the absolute temp delta crosses ChangeThresholdC. */
	UPROPERTY(BlueprintAssignable, Category="ThermoForge|Events")
	FOnHeatJump OnHeatJump;

	/** Fire one initial OnHeatUpdated at BeginPlay (useful to set materials immediately). */
	UPROPERTY(EditAnywhere, Category="ThermoForge|Events")
	bool bFireInitialEventOnBeginPlay = true;

	/** Consider a change “big” if |ΔTemp| >= this value (°C). */
	UPROPERTY(EditAnywhere, Category="ThermoForge|Events", meta=(ClampMin="0.0"))
	float ChangeThresholdC = 10.f;

	/** How we choose origin. */
	UPROPERTY(EditAnywhere, Category="ThermoForge|Origin")
	EThermoOriginMode OriginMode = EThermoOriginMode::NearestSourceActor;

	/** Probe ring radius (cm) for Hottest/Coldest modes. */
	UPROPERTY(EditAnywhere, Category="ThermoForge|Origin", meta=(ClampMin="10.0"))
	float ProbeRadiusCm = 300.f;

	/** Number of probe samples around the ring. */
	UPROPERTY(EditAnywhere, Category="ThermoForge|Origin", meta=(ClampMin="4", ClampMax="64"))
	int32 ProbeSamples = 12;

	/** Update period (seconds). Also triggers on owner transform changes. */
	UPROPERTY(EditAnywhere, Category="ThermoForge|Tick", meta=(ClampMin="0.02"))
	float UpdateRateSec = 0.1f;
	
	UPROPERTY(EditAnywhere, Category="ThermoForge|CPD", meta=(AllowAnyActor))
	FComponentReference OverridePrimitive;

	/** Enable CPD writes into TargetPrim each update. */
	UPROPERTY(EditAnywhere, Category="ThermoForge|CPD")
	bool bWriteCustomPrimitiveData = true;

	/** First CPD float slot (we write 0..8). Reserve 9 slots in your material instance. */
	UPROPERTY(EditAnywhere, Category="ThermoForge|CPD", meta=(ClampMin="0"))
	int32 CPDBaseIndex = 0;

	/** Authoring knob forwarded to CPD[7] (used by your material as a scaler). */
	UPROPERTY(EditAnywhere, Category="ThermoForge|CPD")
	float DistanceIntensity = 1.f;

	/** Authoring knob forwarded to CPD[8] (used by your material as a reference radius). */
	UPROPERTY(EditAnywhere, Category="ThermoForge|CPD", meta=(ClampMin="0.0"))
	float ReferenceRadiusCm = 200.f;

	// --------- Runtime reads (for BP/UI/debug) ---------

	/** Last sampled ambient/grid temperature at owner (°C). */
	UPROPERTY(BlueprintReadOnly, Category="ThermoForge|Runtime")
	float TemperatureC = 0.f;

	/** Direction from owner to chosen origin (unit vector; Zero if unknown). */
	UPROPERTY(BlueprintReadOnly, Category="ThermoForge|Runtime")
	FVector HeatDirWS = FVector::ZeroVector;

	/** Chosen origin position (world). */
	UPROPERTY(BlueprintReadOnly, Category="ThermoForge|Runtime")
	FVector SourcePosWS = FVector::ZeroVector;

	/** Distance (cm) from owner to origin; 0 if unknown. */
	UPROPERTY(BlueprintReadOnly, Category="ThermoForge|Runtime")
	float DistanceCm = 0.f;

	/** Strength proxy (0..∞). For NearestSourceActor it's 1/Distance; for probe modes it’s |ΔT|. */
	UPROPERTY(BlueprintReadOnly, Category="ThermoForge|Runtime")
	float HeatStrength = 0.f;

	/** Last decided origin mode (if changed at runtime). */
	UPROPERTY(BlueprintReadOnly, Category="ThermoForge|Runtime")
	EThermoOriginMode RuntimeOriginMode = EThermoOriginMode::NearestSourceActor;

	/** True if we currently have a valid origin (SourcePosWS set). */
	UFUNCTION(BlueprintCallable, Category="ThermoForge|Runtime")
	bool HasOrigin() const { return bHasOrigin; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Called when owner transform changes (move/rotate/teleport). */
	void HandleTransformUpdated(USceneComponent* Updated, EUpdateTransformFlags, ETeleportType);

	/** Periodic/update-on-move workhorse. */
	void TickHeat();

	/** Resolve target primitive if needed. */
	void EnsureTargetPrimitive();
	UPrimitiveComponent* ResolveOverridePrimitive() const;

	/** Write CPD[0..8] into TargetPrim (if enabled & valid). */
	void WriteCustomPrimitiveData();

	/** Sample grid/system temperature at P (°C). */
	float SampleTempAt(const FVector& P) const;

	/** Origin resolution helpers. Returns true if origin is valid. */
	bool ResolveOrigin_NearestSource(const FVector& CenterWS, FVector& OutOriginWS, float& OutStrength);
	bool ResolveOrigin_Probe(const FVector& CenterWS, bool bFindHottest, FVector& OutOriginWS, float& OutStrength);

private:
	FTimerHandle Timer;
	TWeakObjectPtr<UPrimitiveComponent> TargetPrim;

	float PrevTemperatureC = 0.f;
	FVector PrevSourcePosWS = FVector::ZeroVector;
	FVector PrevHeatDirWS   = FVector::ZeroVector;
	float   PrevDistanceCm  = 0.f;
	float   PrevStrength    = 0.f;

	bool bHasOrigin = false;
	bool bHadInitialFire = false;
};
