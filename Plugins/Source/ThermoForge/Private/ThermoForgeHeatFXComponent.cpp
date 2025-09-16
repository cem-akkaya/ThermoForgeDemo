#include "ThermoForgeHeatFXComponent.h"
#include "ThermoForgeSubsystem.h"
#include "ThermoForgeSourceComponent.h"
#include "ThermoForgeVolume.h"
#include "ThermoForgeFieldAsset.h"
#include "ThermoForgeProjectSettings.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "EngineUtils.h"

static constexpr float TF_EPS_DIR   = 1e-3f;
static constexpr float TF_EPS_TEMP  = 1e-2f;
static constexpr float TF_EPS_DIST  = 0.5f;
static constexpr float TF_EPS_STR   = 1e-3f;

static float TF_SqrDistToAABB(const FVector& P, const FBox& B, FVector* OutClamped = nullptr)
{
	const FVector Q(
		FMath::Clamp(P.X, B.Min.X, B.Max.X),
		FMath::Clamp(P.Y, B.Min.Y, B.Max.Y),
		FMath::Clamp(P.Z, B.Min.Z, B.Max.Z));
	if (OutClamped) *OutClamped = Q;
	return FVector::DistSquared(P, Q);
}

UThermoForgeHeatFXComponent::UThermoForgeHeatFXComponent()
{
	PrimaryComponentTick.bCanEverTick = false; // timer + transform callback
}

void UThermoForgeHeatFXComponent::BeginPlay()
{
	Super::BeginPlay();

	EnsureTargetPrimitive();

	if (AActor* Owner = GetOwner())
	{
		if (USceneComponent* Root = Owner->GetRootComponent())
		{
			Root->TransformUpdated.AddUObject(this, &UThermoForgeHeatFXComponent::HandleTransformUpdated);
		}
	}

	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().SetTimer(
			Timer, this, &UThermoForgeHeatFXComponent::TickHeat,
			UpdateRateSec, /*bLoop=*/true, /*FirstDelay=*/0.0f);
	}

	TickHeat(); // immediate, so materials/events are ready pre-gameplay
}

void UThermoForgeHeatFXComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(Timer);
	}
	Super::EndPlay(EndPlayReason);
}

void UThermoForgeHeatFXComponent::HandleTransformUpdated(USceneComponent* Updated, EUpdateTransformFlags, ETeleportType)
{
	TickHeat();
}

void UThermoForgeHeatFXComponent::EnsureTargetPrimitive()
{
	// Try override first
	if (UPrimitiveComponent* Prim = ResolveOverridePrimitive())
	{
		TargetPrim = Prim;
		return;
	}

	// Fallback: first primitive on owner
	if (AActor* Owner = GetOwner())
	{
		if (UPrimitiveComponent* Found = Owner->FindComponentByClass<UPrimitiveComponent>())
		{
			TargetPrim = Found;
			return;
		}
	}

	TargetPrim = nullptr;
}

UPrimitiveComponent* UThermoForgeHeatFXComponent::ResolveOverridePrimitive() const
{
	if (AActor* Owner = GetOwner())
	{
		if (UActorComponent* Comp = OverridePrimitive.GetComponent(Owner))
		{
			return Cast<UPrimitiveComponent>(Comp);
		}
	}
	return nullptr;
}

float UThermoForgeHeatFXComponent::SampleTempAt(const FVector& P) const
{
	if (const UWorld* W = GetWorld())
	{
		if (const auto* TF = W->GetSubsystem<UThermoForgeSubsystem>())
		{
			// Wire your season/time/weather later as needed
			return TF->ComputeCurrentTemperatureAt(P, /*bWinter=*/false, /*TimeHours=*/12.f, /*WeatherAlpha01=*/0.3f);
		}
	}
	return 0.f;
}

bool UThermoForgeHeatFXComponent::ResolveOrigin_NearestSource(const FVector& CenterWS, FVector& OutOriginWS, float& OutStrength)
{
	float BestDistSq = TNumericLimits<float>::Max();
	FVector BestPos  = FVector::ZeroVector;
	bool bFound = false;

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->FindComponentByClass<UThermoForgeSourceComponent>())
		{
			const FVector Pos = It->GetActorLocation();
			const float Ds = FVector::DistSquared(CenterWS, Pos);
			if (Ds < BestDistSq)
			{
				BestDistSq = Ds;
				BestPos    = Pos;
				bFound     = true;
			}
		}
	}

	if (bFound)
	{
		const float Dist = FMath::Sqrt(BestDistSq);
		OutOriginWS  = BestPos;
		// A simple strength proxy: inverse distance (clamped)
		OutStrength  = (Dist > TF_EPS_DIST) ? (1.f / Dist) : 1.f;
		return true;
	}
	return false;
}

bool UThermoForgeHeatFXComponent::ResolveOrigin_Probe(const FVector& CenterWS, bool bFindHottest, FVector& OutOriginWS, float& OutStrength)
{
	if (UWorld* W = GetWorld())
		if (const auto* TF = W->GetSubsystem<UThermoForgeSubsystem>())
		{
			// UTC is now since other calculations are post process
			FThermoForgeGridHit Hit;
			if (TF->FindBakedExtremeNear(CenterWS, ProbeRadiusCm, bFindHottest, Hit, FDateTime::Now()))
			{
				OutOriginWS = Hit.CellCenterWS;

				// Strength matches your previous semantics: |ΔT| vs center — but using baked-only temps.
				const float TCenter = TF->ComputeBakedOnlyTemperatureAt(CenterWS, /*bWinter=*/false, /*TimeHours=*/12.f, /*WeatherAlpha01=*/0.3f);
				OutStrength = FMath::Abs(Hit.CurrentTempC - TCenter);
				return true;
			}
		}

	// Fallback to old runtime ring probe if no baked field
	const float R   = FMath::Max(ProbeRadiusCm, 10.f);
	const int32 N   = FMath::Clamp(ProbeSamples, 4, 64);
	float BestTemp  = bFindHottest ? -FLT_MAX : +FLT_MAX;
	FVector BestPos = CenterWS;

	const float TCenterRuntime = SampleTempAt(CenterWS); // runtime (sources + ambient)

	for (int32 i = 0; i < N; ++i)
	{
		const float Ang = (2.f * PI) * (float(i) / float(N));
		const FVector P = CenterWS + FVector(FMath::Cos(Ang), FMath::Sin(Ang), 0.f) * R;
		const float T  = SampleTempAt(P);

		if ((bFindHottest && T > BestTemp) || (!bFindHottest && T < BestTemp))
		{ BestTemp = T; BestPos = P; }
	}
	OutOriginWS = BestPos;
	OutStrength = FMath::Abs(BestTemp - TCenterRuntime);
	return true;
}

void UThermoForgeHeatFXComponent::TickHeat()
{
	if (!GetWorld() || !GetOwner()) return;

	const FVector Center = GetOwner()->GetActorLocation();

	// 1) Temperature at owner
	const float TNow = SampleTempAt(Center);

	// 2) Resolve origin based on mode
	FVector OriginWS = FVector::ZeroVector;
	float Strength   = 0.f;
	bool  bOriginOK  = false;
	EThermoOriginMode UsedMode = OriginMode;

	switch (OriginMode)
	{
	case EThermoOriginMode::NearestSourceActor:
		bOriginOK = ResolveOrigin_NearestSource(Center, OriginWS, Strength);
		break;
	case EThermoOriginMode::HottestPoint:
		bOriginOK = ResolveOrigin_Probe(Center, /*bFindHottest=*/true,  OriginWS, Strength);
		break;
	case EThermoOriginMode::ColdestPoint:
		bOriginOK = ResolveOrigin_Probe(Center, /*bFindHottest=*/false, OriginWS, Strength);
		break;
	default:
		bOriginOK = false; break;
	}

	// 3) Build direction/distance
	FVector Dir = FVector::ZeroVector;
	float   Dist= 0.f;
	if (bOriginOK)
	{
		Dir  = (OriginWS - Center).GetSafeNormal();
		Dist = FVector::Distance(Center, OriginWS);
	}

	// 4) Determine if anything “meaningful” changed
	const bool bTempChanged   = !FMath::IsNearlyEqual(TNow, PrevTemperatureC, TF_EPS_TEMP);
	const bool bDirChanged    = !Dir.Equals(PrevHeatDirWS, 1e-3f);
	const bool bPosChanged    = !OriginWS.Equals(PrevSourcePosWS, 0.5f);
	const bool bDistChanged   = !FMath::IsNearlyEqual(Dist, PrevDistanceCm, TF_EPS_DIST);
	const bool bStrChanged    = !FMath::IsNearlyEqual(Strength, PrevStrength, TF_EPS_STR);
	const bool bAnyChanged    = bTempChanged || bDirChanged || bPosChanged || bDistChanged || bStrChanged || !bHadInitialFire;

	// 5) Update state & fire events
	if (bAnyChanged)
	{
		TemperatureC    = TNow;
		SourcePosWS     = OriginWS;
		HeatDirWS       = Dir;
		DistanceCm      = Dist;
		HeatStrength    = Strength;
		RuntimeOriginMode = UsedMode;
		bHasOrigin      = bOriginOK;

		// Big jump?
		const float DeltaC = TNow - PrevTemperatureC;
		if (FMath::Abs(DeltaC) >= ChangeThresholdC)
		{
			OnHeatJump.Broadcast(this, TNow, PrevTemperatureC, DeltaC, HeatStrength, HeatDirWS, DistanceCm, SourcePosWS);
		}

		// Always notify the “updated” event when anything changed (or first fire)
		OnHeatUpdated.Broadcast(this, TemperatureC, HeatStrength, HeatDirWS, DistanceCm, SourcePosWS, RuntimeOriginMode);

		// Cache previous
		PrevTemperatureC = TNow;
		PrevSourcePosWS  = OriginWS;
		PrevHeatDirWS    = Dir;
		PrevDistanceCm   = Dist;
		PrevStrength     = Strength;
		bHadInitialFire  = true;

		// Write CPD
		WriteCustomPrimitiveData();
	}

	// Optional: fire a one-shot at begin play (if nothing changed but user asked)
	if (!bHadInitialFire && bFireInitialEventOnBeginPlay)
	{
		// Pretend an update
		OnHeatUpdated.Broadcast(this, TNow, Strength, Dir, Dist, OriginWS, UsedMode);
		PrevTemperatureC = TNow;
		PrevSourcePosWS  = OriginWS;
		PrevHeatDirWS    = Dir;
		PrevDistanceCm   = Dist;
		PrevStrength     = Strength;
		bHadInitialFire  = true;
		WriteCustomPrimitiveData();
	}
}

void UThermoForgeHeatFXComponent::WriteCustomPrimitiveData()
{
	if (!bWriteCustomPrimitiveData) return;
	EnsureTargetPrimitive();
	if (!TargetPrim.IsValid()) return;

	const int32 I = CPDBaseIndex;

	// [0..2] HeatDirWS  -> Direction
	TargetPrim->SetCustomPrimitiveDataFloat(I + 0, HeatDirWS.X);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 1, HeatDirWS.Y);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 2, HeatDirWS.Z);

	// [3] Temperature (runtime-composed; your existing TNow)
	TargetPrim->SetCustomPrimitiveDataFloat(I + 3, TemperatureC);

	// [4..6] SourcePosWS -> Position (nearest source / hottest / coldest)
	TargetPrim->SetCustomPrimitiveDataFloat(I + 4, SourcePosWS.X);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 5, SourcePosWS.Y);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 6, SourcePosWS.Z);

	// [7] HeatStrength
	TargetPrim->SetCustomPrimitiveDataFloat(I + 7, HeatStrength);

	// [8] ReferenceRadiusCm
	TargetPrim->SetCustomPrimitiveDataFloat(I + 8, ReferenceRadiusCm);

	// Will write zeros if no baked volume is found.
	float  BakedOwnerTempC    = 0.f;
	FVector BakedCellCenterWS = FVector::ZeroVector;
	float  BakedCellTempC     = 0.f;
	float  BakedSky01         = 0.f;
	float  BakedCellSizeCm    = 0.f;

	if (UWorld* W = GetWorld())
	{
		// Pick the best baked volume (prefer inside; else nearest AABB)
		AThermoForgeVolume* BestVol = nullptr;
		float BestSqr = TNumericLimits<float>::Max();

		const FVector OwnerPos = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;

		for (TActorIterator<AThermoForgeVolume> It(W); It; ++It)
		{
			AThermoForgeVolume* V = *It;
			if (!IsValid(V) || !V->BakedField) continue;

			const FBox B = V->GetWorldBounds();
			FVector Clamped;
			const float S = TF_SqrDistToAABB(OwnerPos, B, &Clamped);

			// This can be tweaked further to get the smallest grid maybe
			// Prefer containing volumes (S == 0), else nearest by AABB distance
			const bool bBetter = (S < BestSqr) || (FMath::IsNearlyEqual(S, BestSqr) && BestVol == nullptr);
			if (bBetter) { BestVol = V; BestSqr = S; }
		}

		if (BestVol && BestVol->BakedField)
		{
			UThermoForgeFieldAsset* Field = BestVol->BakedField;
			const FIntVector D = Field->Dim;
			if (D.X > 0 && D.Y > 0 && D.Z > 0)
			{
				const FTransform Frame    = BestVol->GetGridFrame();
				const FTransform InvFrame = Frame.Inverse();
				const float Cell          = FMath::Max(1.f, BestVol->GetEffectiveCellSize());

				// ----- baked temp at owner (Ambient + Solar*Sky(owner)) -----
				const FVector OwnerLS      = InvFrame.TransformPosition(OwnerPos);
				const FVector OwnerCellXYZ = OwnerLS / Cell;

				// Sample SkyView at the owner's position (trilinear from the baked field)
				const float SkyOwner = FMath::Clamp(Field->SampleSkyView01(OwnerPos), 0.f, 1.f);

				// Compose baked-only temp
				const UThermoForgeSubsystem* TF = W->GetSubsystem<UThermoForgeSubsystem>();
				const UThermoForgeProjectSettings* S = TF ? TF->GetSettings() : GetDefault<UThermoForgeProjectSettings>();
				const bool  bWinter     = false;
				const float TimeHours   = 12.f;
				const float WeatherAlfa = S ? S->DefaultWeatherAlpha01 : 0.3f;

				const float AmbientOwnerC = S ? S->GetAmbientCelsiusAt(bWinter, TimeHours, OwnerPos.Z) : 0.f;
				const float SolarOwnerC   = (S ? S->SolarGainScaleC : 6.f) * SkyOwner * (1.f - WeatherAlfa);
				BakedOwnerTempC           = AmbientOwnerC + SolarOwnerC;

				// ----- nearest baked cell center -----
				// Map to nearest CENTER: round((x/cell) - 0.5)
				const int32 ix = FMath::Clamp(FMath::RoundToInt(OwnerCellXYZ.X - 0.5f), 0, D.X - 1);
				const int32 iy = FMath::Clamp(FMath::RoundToInt(OwnerCellXYZ.Y - 0.5f), 0, D.Y - 1);
				const int32 iz = FMath::Clamp(FMath::RoundToInt(OwnerCellXYZ.Z - 0.5f), 0, D.Z - 1);

				const FVector CellCenterLS((ix + 0.5f) * Cell, (iy + 0.5f) * Cell, (iz + 0.5f) * Cell);
				BakedCellCenterWS = Frame.TransformPosition(CellCenterLS);
				BakedCellSizeCm   = Cell;

				const int32 Lin = Field->Index(ix, iy, iz);
				BakedSky01       = FMath::Clamp(Field->GetSkyViewByLinearIdx(Lin), 0.f, 1.f);

				// Baked-only temp at that cell's center
				const float AmbientCellC = S ? S->GetAmbientCelsiusAt(bWinter, TimeHours, BakedCellCenterWS.Z) : 0.f;
				const float SolarCellC   = (S ? S->SolarGainScaleC : 6.f) * BakedSky01 * (1.f - WeatherAlfa);
				BakedCellTempC           = AmbientCellC + SolarCellC;
			}
		}
	}

	// [9]  baked temp at owner
	TargetPrim->SetCustomPrimitiveDataFloat(I + 9,  BakedOwnerTempC);

	// [10..12] baked nearest cell center (WS)
	TargetPrim->SetCustomPrimitiveDataFloat(I + 10, BakedCellCenterWS.X);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 11, BakedCellCenterWS.Y);
	TargetPrim->SetCustomPrimitiveDataFloat(I + 12, BakedCellCenterWS.Z);

	// [13] baked nearest cell temp
	TargetPrim->SetCustomPrimitiveDataFloat(I + 13, BakedCellTempC);

	// [14] baked nearest cell SkyView01
	TargetPrim->SetCustomPrimitiveDataFloat(I + 14, BakedSky01);

	// [15] baked cell size (cm)
	TargetPrim->SetCustomPrimitiveDataFloat(I + 15, BakedCellSizeCm);
}
