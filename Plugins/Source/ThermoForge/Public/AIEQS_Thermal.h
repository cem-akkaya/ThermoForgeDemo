#pragma once

#include "CoreMinimal.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "AIEQS_Thermal.generated.h"

/**
 * Scores items by Thermo Forge temperature (°C) at the item location.
 * Filter: keep items >= MinTempC and <= MaxTempC (if you set them).
 * Score: higher temperature => higher score (or invert via ScoringEquation).
 */

UENUM(BlueprintType)
enum class EThermalScoreMode : uint8
{
	BandPass    UMETA(DisplayName="Prefer Center of Range"),
	HotterBetter UMETA(DisplayName="Hotter is Better"),
	ColderBetter UMETA(DisplayName="Colder is Better")
};


UCLASS(meta=(DisplayName="Thermo Forge: Temperature (°C)"), ClassGroup=AI, BlueprintType)
class THERMOFORGE_API UAIEQS_Thermal : public UEnvQueryTest
{
	GENERATED_BODY()
public:
	UAIEQS_Thermal();

protected:
	virtual void RunTest(FEnvQueryInstance& QueryInstance) const override;

	UPROPERTY(EditDefaultsOnly, Category="Thermo|Scoring")
	EThermalScoreMode ScoreMode = EThermalScoreMode::BandPass;

	/** If > 0, maps the band [Min,Max] to a peak at the center with soft falloff. */
	UPROPERTY(EditDefaultsOnly, Category="Thermo|Scoring", meta=(ClampMin="0.01"))
	float BandHalfWidthC = 2.0f; 
	
	/** If true, line-of-sight from Querier to the item is required for scoring/filter. */
	UPROPERTY(EditDefaultsOnly, Category="Thermo Forge: Temperature (°C)")
	bool bRequireLineOfSight = false;

	/** Min/Max filter in °C. Leave both at -100 to disable explicit filtering in the test. */
	UPROPERTY(EditDefaultsOnly, Category="Thermo Forge: Temperature (°C)", meta=(ToolTip="Minimum temperature (°C) to keep. Set to <= -100 to ignore."))
	float FloatFilterMin = -100;

	UPROPERTY(EditDefaultsOnly, Category="Thermo Forge: Temperature (°C)", meta=(ToolTip="Maximum temperature (°C) to keep. Set to <= -100 to ignore."))
	float FloatFilterMax = -100;

	/** Number of segments for multi-step LOS; 1 = single trace. */
	UPROPERTY(EditDefaultsOnly, Category="Thermo Forge: Temperature (°C)", meta=(EditCondition="bRequireLineOfSight", ClampMin="1", UIMin="1"))
	int32 LoSSteps = 1;
};
