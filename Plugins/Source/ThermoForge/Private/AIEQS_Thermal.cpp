#include "AIEQS_Thermal.h"

#include "EnvironmentQuery/Items/EnvQueryItemType_Point.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "EnvironmentQuery/EnvQueryInstanceBlueprintWrapper.h"
#include "ThermoForgeSubsystem.h"
#include "ThermoForgeProjectSettings.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

UAIEQS_Thermal::UAIEQS_Thermal()
{
	// EQS knows how 
	Cost = EEnvTestCost::Low;
	ValidItemType = UEnvQueryItemType_Point::StaticClass();  // scoring points in space
	SetWorkOnFloatValues(true);                                
}

static bool HasLineOfSightMulti(UWorld* World, const FVector& From, const FVector& To, int32 Steps)
{
	if (!World || Steps <= 0) return true;
	FHitResult Hit;
	const FVector Delta = (To - From) / float(Steps);
	FVector A = From;
	for (int32 i=0; i<Steps; ++i)
	{
		const FVector B = (i+1 == Steps) ? To : (A + Delta);
		if (World->LineTraceSingleByChannel(Hit, A, B, ECC_Visibility))
			return false;
		A = B;
	}
	return true;
}

void UAIEQS_Thermal::RunTest(FEnvQueryInstance& QueryInstance) const
{
	UObject* QueryOwnerObj = QueryInstance.Owner.Get();
	UWorld*  World         = QueryOwnerObj ? QueryOwnerObj->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	// Thermo subsystem
	UThermoForgeSubsystem* Thermo = World->GetSubsystem<UThermoForgeSubsystem>();
	if (!Thermo)
	{
		return;
	}

	// For LOS and time-of-day composition, we’ll use the current time (UTC)
	const FDateTime NowUTC = FDateTime::UtcNow();

	// Pull item data as points
	const UEnvQueryItemType_Point* PointTypeCDO = GetDefault<UEnvQueryItemType_Point>();
	if (!PointTypeCDO) return;

	// Read test purpose (filter vs score) and filter clamp from UEnvQueryTest base
	const bool bUseExplicitFilter =
		(FloatFilterMin > -100.f) || (FloatFilterMax > -100.f);

	// Iterate items
	for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It)
	{
		const FVector ItemLoc = GetItemLocation(QueryInstance, It.GetIndex());

		//  LOS
		if (bRequireLineOfSight)
		{
			const AActor* Querier = Cast<AActor>(QueryInstance.Owner.Get());
			const FVector From = Querier ? Querier->GetActorLocation() : ItemLoc;
			if (!HasLineOfSightMulti(World, From, ItemLoc, FMath::Max(1, LoSSteps)))
			{
				It.ForceItemState(EEnvItemStatus::Failed);
				continue;
			}
		}

		// Compose current temperature using Thermo Forge
		const FThermoForgeGridHit Hit = Thermo->QueryNearestBakedGridPoint(ItemLoc, NowUTC);
		if (!Hit.bFound)
		{
			It.ForceItemState(EEnvItemStatus::Failed);
			continue;
		}

		const float TempC = Hit.CurrentTempC;

		// Filtering
		float Score = 0.f;

		switch (ScoreMode)
		{
		case EThermalScoreMode::HotterBetter:
			Score = TempC; 
			break;

		case EThermalScoreMode::ColderBetter:
			Score = -TempC;
			break;

		case EThermalScoreMode::BandPass:
		default:
			{
				// prefer the center falloff.
				const bool bHasMin = (FloatFilterMin > -100.f);
				const bool bHasMax = (FloatFilterMax > -100.f);
				const float MinC = bHasMin ? FloatFilterMin : TempC - BandHalfWidthC;
				const float MaxC = bHasMax ? FloatFilterMax : TempC + BandHalfWidthC;

				const float Center = 0.5f * (MinC + MaxC);
				const float Half   = FMath::Max(0.001f, 0.5f * (MaxC - MinC)); // avoid div by 0

				const float t = FMath::Clamp(1.f - FMath::Abs(TempC - Center) / Half, 0.f, 1.f);
				
				const float sharpen = FMath::Clamp(Half / FMath::Max(0.001f, BandHalfWidthC), 0.25f, 4.f);
				Score = FMath::Pow(t, sharpen) * 100.f; // 0..100
				break;
			}
		}

		It.SetScore(TestPurpose, FilterType, Score, 0.f, 100.f);

	}
}
