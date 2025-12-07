#include "ThermoForgeSubsystem.h"
#include "ThermoForgeProjectSettings.h"
#include "ThermoForgeFieldAsset.h"
#include "ThermoForgeVolume.h"
#include "ThermoForgeSourceComponent.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "DrawDebugHelpers.h"
#include "Components/InstancedStaticMeshComponent.h"

#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "PhysicsEngine/BodySetup.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "PackageTools.h"
#endif

// ---- settings access ----
const UThermoForgeProjectSettings* UThermoForgeSubsystem::GetSettings() const
{
    return GetDefault<UThermoForgeProjectSettings>();
}

void UThermoForgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
}

void UThermoForgeSubsystem::Deinitialize()
{
    SourceSet.Empty();
    Super::Deinitialize();
}

void UThermoForgeSubsystem::RegisterSource(UThermoForgeSourceComponent* Source)
{
    if (!IsValid(Source)) return;
    SourceSet.Add(Source);
    CompactSources();
    OnSourcesChanged.Broadcast();
}

void UThermoForgeSubsystem::UnregisterSource(UThermoForgeSourceComponent* Source)
{
    if (!Source) return;
    SourceSet.Remove(Source);
    CompactSources();
    OnSourcesChanged.Broadcast();
}

void UThermoForgeSubsystem::MarkSourceDirty(UThermoForgeSourceComponent* /*Source*/)
{
    OnSourcesChanged.Broadcast();
}

int32 UThermoForgeSubsystem::GetSourceCount() const
{
    int32 Count = 0;
    for (const TWeakObjectPtr<UThermoForgeSourceComponent>& W : SourceSet)
        if (W.IsValid()) ++Count;
    return Count;
}
// Actual Baking per volume direction bounds
void UThermoForgeSubsystem::StartNextBake()
{
    // Nothing left to bake
    if (BakeQueue.Num() == 0)
    {
        BakeVolume = nullptr;
        return;
    }

    // Pop the first valid volume
    AThermoForgeVolume* V = nullptr;
    while (BakeQueue.Num() > 0 && !V)
    {
        V = BakeQueue[0].Get();
        BakeQueue.RemoveAt(0);
    }

    // If no valid volume was found, stop
    if (!V)
    {
        BakeVolume = nullptr;
        return;
    }

    BakeVolume = V;

    // Rotations and orientation of cell generation
    const FQuat GridRot = V->GetGridFrame().GetRotation();     
    const FVector Pivot  = V->GetActorLocation();              
    const FTransform Frame(GridRot, Pivot);
    
    const FTransform InvFrame = Frame.Inverse();
    const FBox Bounds = V->GetWorldBounds();
    const float Cell   = V->GetEffectiveCellSize();

    // Grid box in local space
    FVector Corners[8] = {
        FVector(Bounds.Min.X, Bounds.Min.Y, Bounds.Min.Z),
        FVector(Bounds.Min.X, Bounds.Min.Y, Bounds.Max.Z),
        FVector(Bounds.Min.X, Bounds.Max.Y, Bounds.Min.Z),
        FVector(Bounds.Min.X, Bounds.Max.Y, Bounds.Max.Z),
        FVector(Bounds.Max.X, Bounds.Min.Y, Bounds.Min.Z),
        FVector(Bounds.Max.X, Bounds.Min.Y, Bounds.Max.Z),
        FVector(Bounds.Max.X, Bounds.Max.Y, Bounds.Min.Z),
        FVector(Bounds.Max.X, Bounds.Max.Y, Bounds.Max.Z),
    };
    FBox GridBox(ForceInit);
    for (int i=0; i<8; ++i)
    {
        GridBox += InvFrame.TransformPosition(Corners[i]);
    }

    auto FloorDiv0 = [](double X, double Step)->int32 { return FMath::FloorToInt(X / Step); };
    auto CeilDiv0  = [](double X, double Step)->int32 { return FMath::CeilToInt(X / Step); };

    const int32 ix0 = FloorDiv0(GridBox.Min.X, Cell);
    const int32 iy0 = FloorDiv0(GridBox.Min.Y, Cell);
    const int32 iz0 = FloorDiv0(GridBox.Min.Z, Cell);

    const int32 ix1 = CeilDiv0 (GridBox.Max.X, Cell) - 1;
    const int32 iy1 = CeilDiv0 (GridBox.Max.Y, Cell) - 1;
    const int32 iz1 = CeilDiv0 (GridBox.Max.Z, Cell) - 1;

    FIntVector Dim(
        FMath::Max(0, ix1 - ix0 + 1),
        FMath::Max(0, iy1 - iy0 + 1),
        FMath::Max(0, iz1 - iz0 + 1)
    );

    const int32 N  = Dim.X * Dim.Y * Dim.Z;
    if (N <= 0) { StartNextBake(); return; }

    // save state
    BakeVolume = V;
    BakeFrame = Frame;
    BakeCell = Cell;
    Bake_ix0 = ix0;
    Bake_iy0 = iy0;
    Bake_iz0 = iz0;
    BakeDim = Dim;

    // now compute the correct world origin of cell (ix0,iy0,iz0) in THIS volume's frame
    BakeFieldOriginWS = Frame.TransformPosition(FVector(
        ix0 * Cell,
        iy0 * Cell,
        iz0 * Cell
    ));
    
    BakeTotalCells = N;
    BakeProcessed = 0;

    BakeSky.SetNumZeroed(N);
    BakeWall.SetNumZeroed(N);
    BakeIndoor.SetNumZeroed(N);

    GetWorld()->GetTimerManager().SetTimer(BakeTimerHandle, this,
        &UThermoForgeSubsystem::TickBake, 0.01f, true);

    OnBakeProgress.Broadcast(0.f);
}


void UThermoForgeSubsystem::GetAllSources(TArray<UThermoForgeSourceComponent*>& OutSources) const
{
    OutSources.Reset();
    for (const TWeakObjectPtr<UThermoForgeSourceComponent>& W : SourceSet)
        if (UThermoForgeSourceComponent* S = W.Get())
            OutSources.Add(S);
}

static void TF_GenerateBlueNoiseSphere(TArray<FVector>& Out, int32 Count, float RadiusCm, uint32 Seed=1337u)
{
    Out.Reset(Count);
    Out.Reserve(Count);

    FRandomStream R(Seed);

    const int32 DomeCount = Count * 3 / 4; 
    const int32 ZCount    = Count - DomeCount;

    // Hemisphere (upper half-sphere, Z ≥ 0)
    for (int32 i=0; i<DomeCount; ++i)
    {
        const float k   = (i + 0.5f) / float(DomeCount);
        const float phi = 2.f * PI * k * 1.61803398875f;
        const float z   = FMath::FRandRange(0.f, 1.f);  // only upper half
        const float r   = FMath::Sqrt(FMath::Max(0.f, 1.f - z*z));

        FVector p(r * FMath::Cos(phi), r * FMath::Sin(phi), z);
        p.Normalize();

        const float u = R.FRand();
        const float radius = FMath::Lerp(0.3f * RadiusCm, RadiusCm, FMath::Sqrt(u));

        Out.Add(p * radius);
    }

    // Vertical stack (extra Z axis coverage)
    for (int32 j=0; j<ZCount; ++j)
    {
        float z = FMath::Lerp(-1.f, 1.f, (j+0.5f) / float(ZCount));
        FVector p(0, 0, z);
        p.Normalize();

        const float u = R.FRand();
        const float radius = FMath::Lerp(0.3f * RadiusCm, RadiusCm, FMath::Sqrt(u));

        Out.Add(p * radius);
    }
}

void UThermoForgeSubsystem::UpdateThermalProbesAndUpload(const FVector& CenterWS, bool bRegeneratePoints)
{
    if (MaxProbes <= 0) return;

    // 1) Ensure probe positions (relative to CenterWS)
    if (bRegeneratePoints || ProbeOffsetsLS.Num() != MaxProbes)
    {
        TF_GenerateBlueNoiseSphere(ProbeOffsetsLS, MaxProbes, ProbeRadiusCm, /*Seed*/ 98761u);
    }
    NumProbes = ProbeOffsetsLS.Num();

    // 2) Evaluate temperature at each probe
    if (ProbePixels.Num() != NumProbes)
    {
        ProbePixels.SetNum(NumProbes);
    }

    // For Default expose these if you want control over it in blueprints or gameplay
    const bool  bWinter     = false;
    const float TimeHours   = 12.f;         // “reference” time
    const float WeatherAlfa = 0.2f;         // clearer = lower alpha

    for (int32 i=0; i<NumProbes; ++i)
    {
        const FVector P = CenterWS + ProbeOffsetsLS[i];

        // Use your existing runtime composition (includes ambient + solar + sources + occlusion):
        const float TempC = ComputeCurrentTemperatureAt(P, bWinter, TimeHours, WeatherAlfa);

        // Pack as RGBA16F = (RelX, RelY, RelZ, TempC)
        const FVector Rel = ProbeOffsetsLS[i]; // already relative to CenterWS
        ProbePixels[i] = FFloat16Color(FLinearColor(Rel.X, Rel.Y, Rel.Z, TempC));
    }

    // 3) Ensure / update the transient 2D texture (width = NumProbes, height = 1)
    const int32 W = NumProbes;
    const int32 H = 1;

    const EPixelFormat PF = PF_FloatRGBA;

    if (!ProbesTex || ProbesTex->GetPixelFormat() != PF || ProbesTex->GetSizeX() != W || ProbesTex->GetSizeY() != H)
    {
        ProbesTex = UTexture2D::CreateTransient(W, H, PF);
        ProbesTex->AddressX = TA_Clamp;
        ProbesTex->AddressY = TA_Clamp;
        ProbesTex->SRGB = false;
        ProbesTex->Filter = TF_Bilinear;
        ProbesTex->CompressionSettings = TC_HDR; // keep HDR linear
#if WITH_EDITORONLY_DATA
        ProbesTex->MipLoadOptions = ETextureMipLoadOptions::AllMips; 
#endif
        ProbesTex->UpdateResource();
    }

    // 4) Upload pixel data
    {
        FTexture2DMipMap& Mip = ProbesTex->GetPlatformData()->Mips[0];
        void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
        const size_t Bytes = size_t(W) * size_t(H) * sizeof(FFloat16Color);
        FMemory::Memcpy(Data, ProbePixels.GetData(), Bytes);
        Mip.BulkData.Unlock();
        ProbesTex->UpdateResource();
    }

    // 5) How TO : Expose to material or you can do in blueprints access these variables. 
    // A) If you have a post-process MID handy:
    //    MID->SetTextureParameterValue("TF_ProbesTex", ProbesTex);
    //    MID->SetScalarParameterValue("TF_NumProbes", float(NumProbes));
    //    MID->SetScalarParameterValue("TF_KernelRadiusCm", KernelRadiusCm);
    //    MID->SetVectorParameterValue("TF_CenterWS", FLinearColor(CenterWS));

    // or B) If you use a Material Parameter Collection (MPC), set scalars/vectors there,
    // and set the texture on your PP material instance.
}


// ---- physmat helpers ----
static UPhysicalMaterial* TF_ResolvePhysicalMaterial(const FHitResult& Hit)
{
    if (UPhysicalMaterial* PM = Hit.PhysMaterial.Get()) return PM;

    if (UPrimitiveComponent* PC = Hit.GetComponent())
    {
        if (PC->BodyInstance.GetSimplePhysicalMaterial())
            return PC->BodyInstance.GetSimplePhysicalMaterial();
        if (UBodySetup* BS = PC->GetBodySetup())
            if (BS->PhysMaterial)
                return BS->PhysMaterial;
    }
    return nullptr;
}

static float TF_GetHitDensityKgM3(const FHitResult& Hit, const UThermoForgeProjectSettings* S)
{
    if (!S) return 1.f;

    if (S->bUsePhysicsMaterialForDensity)
    {
        if (UPhysicalMaterial* PM = TF_ResolvePhysicalMaterial(Hit))
        {
            const float Found = PM->Density;
            return FMath::Max(0.f, Found);
        }
    }
    return S->bTreatMissingPhysMatAsAir ? S->AirDensityKgM3 : S->UnknownHitDensityKgM3;
}

// ---- single ray permeability (Beer–Lambert on hit) ----
float UThermoForgeSubsystem::TraceAmbientRay01(const FVector& P, const FVector& Dir, float MaxLen) const
{
    const UWorld* W = GetWorld();
    const UThermoForgeProjectSettings* S = GetSettings();
    if (!W || !S) return 1.f;

    FHitResult Hit;
    FCollisionQueryParams Q(SCENE_QUERY_STAT(ThermoAmbient), S->bTraceComplex);
    Q.bReturnPhysicalMaterial = true;

    const bool bHit = W->LineTraceSingleByChannel(
        Hit, P, P + Dir * MaxLen,
        static_cast<ECollisionChannel>(S->TraceChannel.GetValue()),
        Q);

    if (!bHit) return 1.f;

    const float rho   = TF_GetHitDensityKgM3(Hit, S);
    const float Lfrac = S->FaceThicknessFactor;
    return S->DensityToPermeability(rho, Lfrac);
}

float UThermoForgeSubsystem::OcclusionBetween(const FVector& A, const FVector& B, float CellSizeCm) const
{
    const UWorld* W = GetWorld();
    const UThermoForgeProjectSettings* S = GetSettings();
    if (!W || !S) return 1.f;

    FHitResult Hit;
    FCollisionQueryParams Q(SCENE_QUERY_STAT(ThermoSource), S->bTraceComplex);
    Q.bReturnPhysicalMaterial = true;

    const bool bHit = W->LineTraceSingleByChannel(
        Hit, A, B,
        static_cast<ECollisionChannel>(S->TraceChannel.GetValue()),
        Q);

    if (!bHit) return 1.f; // open one

    const float rho   = TF_GetHitDensityKgM3(Hit, S);
    const float Dist  = FVector::Distance(A, B);
    const float Cell  = FMath::Max(1.f, CellSizeCm);
    const float Lfrac = (Dist / Cell) * S->FaceThicknessFactor;

    return S->DensityToPermeability(rho, Lfrac);
}

// Climate classifications
EThermoClimateType UThermoForgeSubsystem::ClassifyClimateFromTemperature(float TempC) const
{
    if (TempC <= -5.f)  return EThermoClimateType::Arctic;
    if (TempC <= 5.f)   return EThermoClimateType::Cold;
    if (TempC <= 18.f)  return EThermoClimateType::Temperate;
    if (TempC <= 28.f)  return EThermoClimateType::Warm;
    if (TempC <= 40.f)  return EThermoClimateType::Tropical;

    return EThermoClimateType::Desert;
}

// ---- main bake start: collect volumes and que first ----
// Per volume creates a que, then starts process per volume and tickbake() to calculate actual cell grid.
void UThermoForgeSubsystem::KickstartSamplingFromVolumes()
{
    UWorld* W = GetWorld();
    const UThermoForgeProjectSettings* S = GetSettings();
    if (!W || !S) return;

    // hemisphere dirs
    BakeHemiDirs.Reset();
    {
        const FVector base[12] = {
            { 0, 0, 1}, { 0.5, 0, 0.866f}, {-0.5, 0, 0.866f}, {0, 0.5, 0.866f}, {0, -0.5, 0.866f},
            { 0.707f, 0.707f, 0}, {-0.707f, 0.707f, 0}, {0.707f,-0.707f, 0}, {-0.707f,-0.707f, 0},
            { 0.923f, 0, 0.382f}, {-0.923f, 0, 0.382f}, {0, 0.923f, 0.382f}
        };
        BakeHemiDirs.Append(base, UE_ARRAY_COUNT(base));
        for (FVector& d : BakeHemiDirs) d.Normalize();
    }

    // collect all volumes
    BakeQueue.Empty();
    for (TActorIterator<AThermoForgeVolume> It(W); It; ++It)
    {
        AThermoForgeVolume* V = *It;
        if (V) BakeQueue.Add(V);
    }

    if (!BakeVolume.IsValid() && BakeQueue.Num() > 0)
    {
        StartNextBake();
    }
}

// ---------- Public BP entry: nearest baked cell ----------
bool UThermoForgeSubsystem::VolumeContainsPoint(const AThermoForgeVolume* Vol, const FVector& WorldLocation) const
{
    if (!Vol) return false;
    if (Vol->bUnbounded) return true;

    const FTransform T = Vol->GetActorTransform();
    const FVector L = T.InverseTransformPosition(WorldLocation);
    const FVector Min = -Vol->BoxExtent, Max = Vol->BoxExtent;
    return (L.X >= Min.X && L.X <= Max.X)
        && (L.Y >= Min.Y && L.Y <= Max.Y)
        && (L.Z >= Min.Z && L.Z <= Max.Z);
}

bool UThermoForgeSubsystem::ComputeNearestInVolume(const AThermoForgeVolume* Vol, const FVector& WorldLocation, FThermoForgeGridHit& OutHit) const
{
    if (!Vol || !Vol->BakedField) return false;

    const UThermoForgeFieldAsset* Field = Vol->BakedField;
    const FIntVector D = Field->Dim;
    if (D.X <= 0 || D.Y <= 0 || D.Z <= 0) return false;

    const float Cell = Field->CellSizeCm;
    if (Cell <= 0.f) return false;

    // Use the asset's oriented frame (origin + rotation at bake time)
    const FTransform Frame = Field->GetGridFrame();
    const FTransform InvFrame = Frame.Inverse();

    // Map world → grid-local (in cell units)
    const FVector LocalGrid = InvFrame.TransformPosition(WorldLocation) / Cell;

    // Nearest cell indices in grid space
    const int32 ix = FMath::Clamp(FMath::FloorToInt(LocalGrid.X + 0.5f), 0, D.X - 1);
    const int32 iy = FMath::Clamp(FMath::FloorToInt(LocalGrid.Y + 0.5f), 0, D.Y - 1);
    const int32 iz = FMath::Clamp(FMath::FloorToInt(LocalGrid.Z + 0.5f), 0, D.Z - 1);

    const int32 Nx = D.X, Ny = D.Y;
    const int32 Linear = ix + iy * Nx + iz * Nx * Ny;

    // Reconstruct WORLD cell center via the same frame
    const FVector CellCenterWS = Frame.TransformPosition(
        FVector((ix + 0.5f) * Cell, (iy + 0.5f) * Cell, (iz + 0.5f) * Cell));

    const double DistSq = FVector::DistSquared(CellCenterWS, WorldLocation);

    OutHit.bFound       = true;
    OutHit.Volume       = const_cast<AThermoForgeVolume*>(Vol);
    OutHit.GridIndex    = FIntVector(ix, iy, iz);
    OutHit.LinearIndex  = Linear;
    OutHit.CellCenterWS = CellCenterWS;
    OutHit.DistanceSq   = DistSq;
    OutHit.CellSizeCm   = Cell;
    return true;
}



FThermoForgeGridHit UThermoForgeSubsystem::QueryNearestBakedGridPoint(const FVector& WorldLocation, const FDateTime& QueryTimeUTC) const
{
    FThermoForgeGridHit Best;

    UWorld* World = GetWorld();
    if (!World) return Best;

    bool FoundInContaining = false;

    for (TActorIterator<AThermoForgeVolume> It(World); It; ++It)
    {
        const AThermoForgeVolume* Vol = *It;
        if (!Vol || !Vol->BakedField) continue;

        if (!VolumeContainsPoint(Vol, WorldLocation))
            continue;

        FThermoForgeGridHit Hit;
        if (ComputeNearestInVolume(Vol, WorldLocation, Hit))
        {
            Hit.QueryTimeUTC = QueryTimeUTC;
            if (!FoundInContaining || Hit.DistanceSq < Best.DistanceSq)
            {
                Best = Hit;
                FoundInContaining = true;
            }
        }
    }

    if (!FoundInContaining)
    {
        for (TActorIterator<AThermoForgeVolume> It(World); It; ++It)
        {
            const AThermoForgeVolume* Vol = *It;
            if (!Vol || !Vol->BakedField) continue;

            FThermoForgeGridHit Hit;
            if (ComputeNearestInVolume(Vol, WorldLocation, Hit))
            {
                Hit.QueryTimeUTC = QueryTimeUTC;
                if (!Best.bFound || Hit.DistanceSq < Best.DistanceSq)
                {
                    Best = Hit;
                }
            }
        }
    }

   // Fill composed temperature (derived from QueryTimeUTC) with post-process ambient fix
if (Best.bFound)
{
    const UThermoForgeProjectSettings* S = GetSettings();

    // Weather alpha: keep using the preview knob @To do - Live Preview Time Taps here
    const float WeatherAlfa = S ? S->PreviewWeatherAlpha : 0.3f;

    // --- Time of day from UTC (continuous hours) ---
    const double SecUTC   = Best.QueryTimeUTC.GetTimeOfDay().GetTotalSeconds();
    auto Wrap24 = [](float h){ float r = FMath::Fmod(h, 24.f); return (r < 0.f) ? r + 24.f : r; };
    const float TimeHours = ([](float h){ float r = FMath::Fmod(h, 24.f); return (r < 0.f) ? r + 24.f : r; })
                            (static_cast<float>(SecUTC / 3600.0f));

    // --- Smooth seasonal alpha (0 = deep winter, 1 = peak summer), Northern Hemisphere ---
    // Dec 21 (~355) -> 0, Jun 21 (~172) -> 1, smooth cosine over the year
    auto Wrap01 = [](float x){ return x - FMath::FloorToFloat(x); };
    const int32 DOY           = Best.QueryTimeUTC.GetDayOfYear();      // 1..365/366
    const float YearPos       = Wrap01((float(DOY) - 355.0f) / 365.0f); // 0..1 starting at Dec 21
    const float SeasonAlpha01 = 0.5f * (1.0f - FMath::Cos(2.0f * PI * YearPos)); // 0→1→0 yearly

    // --- Compute BASELINE using the SAME season (blend winter/summer) to avoid steps ---
    // Seasons Blend
    const float BaseWinter = ComputeCurrentTemperatureAt(Best.CellCenterWS, /*bWinter=*/true , TimeHours, WeatherAlfa);
    const float BaseSummer = ComputeCurrentTemperatureAt(Best.CellCenterWS, /*bWinter=*/false, TimeHours, WeatherAlfa);
    const float BaselineTotalC = FMath::Lerp(BaseWinter, BaseSummer, SeasonAlpha01);

    // Extract the ambient part used by baseline (also blended), so we can phase-correct it:
    const float AmbWinter = S ? S->GetAmbientCelsiusAt(/*bWinter=*/true , TimeHours, Best.CellCenterWS.Z) : 0.0f;
    const float AmbSummer = S ? S->GetAmbientCelsiusAt(/*bWinter=*/false, TimeHours, Best.CellCenterWS.Z) : 0.0f;
    const float BaselineAmbientC = FMath::Lerp(AmbWinter, AmbSummer, SeasonAlpha01);

    // --- Desired ambient with new phase: coldest at 00:00, hottest at 12:00 (also seasonal blend) ---
    const float WinterAvg   = S ? S->WinterAverageC       : 5.0f;
    const float SummerAvg   = S ? S->SummerAverageC       : 28.0f;
    const float WinterDelta = S ? S->WinterDayNightDeltaC : 8.0f;
    const float SummerDelta = S ? S->SummerDayNightDeltaC : 10.0f;

    const float AvgC   = FMath::Lerp(WinterAvg,   SummerAvg,   SeasonAlpha01);
    const float DeltaC = FMath::Lerp(WinterDelta, SummerDelta, SeasonAlpha01);

    // 00:00 trough, 12:00 peak
    const float Phase   = (TimeHours - 12.0f) / 24.0f;
    const float CosWave = FMath::Cos(2.0f * PI * Phase);
    const float DesiredAmbientSeaC = AvgC + 0.5f * DeltaC * CosWave;

    // Altitude adjustment
    const float DesiredAmbientC = S ? S->AdjustForAltitude(DesiredAmbientSeaC, Best.CellCenterWS.Z)
                                    : DesiredAmbientSeaC;

    // --- Apply phase correction without jumps ---
    const float AmbientDelta = DesiredAmbientC - BaselineAmbientC;
    Best.CurrentTempC = BaselineTotalC + AmbientDelta;

}


    return Best;
}

FThermoForgeGridHit UThermoForgeSubsystem::QueryNearestBakedGridPointNow(const FVector& WorldLocation) const
{
    return QueryNearestBakedGridPoint(WorldLocation, FDateTime::UtcNow());
}

EThermoClimateType UThermoForgeSubsystem::GetClimateTypeAtPoint(const FVector& WorldPos) const
{
    const UThermoForgeProjectSettings* Settings = GetSettings();
    if (!Settings)
        return EThermoClimateType::Temperate;

    // Try sample if there is a volume at that point since its the source of truth
    const bool  bWinter     = Settings->PreviewSeasonIsWinter;
    const float TimeHours   = Settings->PreviewTimeOfDayHours;
    const float WeatherAlfa = Settings->PreviewWeatherAlpha;

    const float TempC = ComputeCurrentTemperatureAt(WorldPos, bWinter, TimeHours, WeatherAlfa);

    // If the runtime system returns any meaningful temperature, classify now.
    if (!FMath::IsNaN(TempC))
    {
        return ClassifyClimateFromTemperature(TempC);
    }
    // if there is no data we model the overall climate temperature at the point by falloff since it would be the most accurate model of climate.

    float AmbientC = Settings->GetAmbientCelsiusAt(bWinter, TimeHours, WorldPos.Z);

    return ClassifyClimateFromTemperature(AmbientC);
}

// --------- Runtime composition ---------
float UThermoForgeSubsystem::ComputeCurrentTemperatureAt(const FVector& WorldPos, bool bWinter, float TimeHours, float WeatherAlpha01) const
{
    const UThermoForgeProjectSettings* S = GetSettings();
    if (!S) return 0.f;

    // Find nearest baked field and read both scalars
    float Sky = 0.f;
    float WallPerm = 1.f;

    {
        UWorld* World = GetWorld();
        FThermoForgeGridHit Best;
        if (World)
        {
            for (TActorIterator<AThermoForgeVolume> It(World); It; ++It)
            {
                const AThermoForgeVolume* Vol = *It;
                if (!Vol || !Vol->BakedField) continue;

                FThermoForgeGridHit Hit;
                if (!ComputeNearestInVolume(Vol, WorldPos, Hit)) continue;

                if (!Best.bFound || Hit.DistanceSq < Best.DistanceSq)
                    Best = Hit;
            }
        }

        if (Best.bFound && Best.Volume && Best.Volume->BakedField)
        {
            Sky      = FMath::Clamp(Best.Volume->BakedField->GetSkyViewByLinearIdx(Best.LinearIndex), 0.f, 1.f);
            WallPerm = FMath::Clamp(Best.Volume->BakedField->GetWallPermByLinearIdx(Best.LinearIndex), 0.f, 1.f);
        }
    }

    // Ambient + altitude
    float AmbientC = S->GetAmbientCelsiusAt(bWinter, TimeHours, WorldPos.Z);

    // Solar gain (reduced by weather)
    const float Solar = S->SolarGainScaleC * Sky * (1.f - FMath::Clamp(WeatherAlpha01, 0.f, 1.f));

    // Dynamic sources (attenuated by LOS * local wall permeability)
    float SourceSum = 0.f;
    {
        for (const TWeakObjectPtr<UThermoForgeSourceComponent>& W : SourceSet)
        {
            const UThermoForgeSourceComponent* Sc = W.Get();
            if (!Sc || !Sc->bEnabled) continue;

            const float Intensity = Sc->SampleAt(WorldPos); // °C delta
            if (Intensity == 0.f) continue;

            const float CellSize = S->DefaultCellSizeCm;
            const float Occ = OcclusionBetween(WorldPos, Sc->GetOwnerLocationSafe(), CellSize);
            // WallPerm scales local transmissivity
            SourceSum += Intensity * Occ * WallPerm;
        }
    }

    return AmbientC + Solar + SourceSum;
}

// ---- Save helpers ----
#if WITH_EDITOR
UThermoForgeFieldAsset* UThermoForgeSubsystem::CreateAndSaveFieldAsset(AThermoForgeVolume* Volume,
    const FIntVector& Dim, float Cell, const FVector& FieldOriginWS, const FRotator& GridRotation,
    const TArray<float>& SkyView01, const TArray<float>& WallPerm01, const TArray<float>& Indoor01) const
{
    if (!Volume) return nullptr;

    const FString VolName     = Volume->GetName();
    const FString PackageName = FString::Printf(TEXT("/Game/ThermoForge/Bakes/%s_Field"), *VolName);
    const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);

    UPackage* Pkg = CreatePackage(*PackageName);
    Pkg->FullyLoad();

    UThermoForgeFieldAsset* Saved = FindObject<UThermoForgeFieldAsset>(Pkg, *AssetName);
    const bool bIsNew = (Saved == nullptr);
    if (!Saved)
    {
        Saved = NewObject<UThermoForgeFieldAsset>(Pkg, UThermoForgeFieldAsset::StaticClass(),
                                                  *AssetName, RF_Public | RF_Standalone);
        FAssetRegistryModule::AssetCreated(Saved);
    }

    Saved->Dim               = Dim;
    Saved->CellSizeCm        = Cell;
    Saved->OriginWS          = FieldOriginWS;
    Saved->GridRotation = GridRotation;
    Saved->SkyView01         = SkyView01;
    Saved->WallPermeability01= WallPerm01;
    Saved->Indoorness01      = Indoor01;

    Saved->MarkPackageDirty();
    Pkg->MarkPackageDirty();

    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags     = ESaveFlags::SAVE_None;
    SaveArgs.Error         = GWarn;

    const bool bOk = UPackage::SavePackage(Pkg, Saved, *Filename, SaveArgs);
    UE_LOG(LogTemp, Log, TEXT("[ThermoForge] Asset %s : %s"),
           *Filename, bOk ? TEXT("Saved") : TEXT("FAILED"));

    if (!bOk) return nullptr;

    return Saved;
}
#endif

void UThermoForgeSubsystem::TF_DumpFieldToSavedFolder(const FString& VolName, const FIntVector& Dim, float Cell,
    const FVector& OriginWS, const TArray<float>& SkyView01, const TArray<float>& WallPerm01, const TArray<float>& Indoor01)
{
#if WITH_EDITOR
    const FString Dir  = FPaths::ProjectSavedDir() / TEXT("ThermoForge/Bakes");
    IFileManager::Get().MakeDirectory(*Dir, true);

    const FString Fn   = FString::Printf(TEXT("%s/%s_Field_%s.csv"),
                        *Dir, *VolName, *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));

    FString Out;
    Out.Reserve(256 + SkyView01.Num() * 48);
    Out += TEXT("# DimX,DimY,DimZ,CellSizeCm,OriginX,OriginY,OriginZ\n");
    Out += FString::Printf(TEXT("%d,%d,%d,%.6f,%.6f,%.6f,%.6f\n"),
                           Dim.X, Dim.Y, Dim.Z, Cell, OriginWS.X, OriginWS.Y, OriginWS.Z);
    Out += TEXT("index,skyview,wallperm,indoor\n");

    const int32 N = SkyView01.Num();
    for (int32 i=0; i<N; ++i)
    {
        const float A = SkyView01.IsValidIndex(i) ? SkyView01[i] : 0.f;
        const float B = WallPerm01.IsValidIndex(i) ? WallPerm01[i] : 1.f;
        const float C = Indoor01.IsValidIndex(i) ? Indoor01[i] : 0.f;
        Out += FString::Printf(TEXT("%d,%.6f,%.6f,%.6f\n"), i, A, B, C);
    }

#endif
}

void UThermoForgeSubsystem::CompactSources()
{
    for (auto It = SourceSet.CreateIterator(); It; ++It)
    {
        if (!It->IsValid())
        {
            It.RemoveCurrent();
        }
        else
        {
            const UThermoForgeSourceComponent* S = It->Get();
            if (!S->GetWorld())
            {
                It.RemoveCurrent();
            }
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[ThermoForge] Compacted %d sources"), SourceSet.Num());
}



float UThermoForgeSubsystem::ComputeBakedOnlyTemperatureAt(const FVector& WorldPos, bool bWinter, float TimeHours, float WeatherAlpha01) const
{
    const UThermoForgeProjectSettings* S = GetSettings();
    if (!S) return 0.f;

    // Pick nearest baked cell (prefer containing volume).
    FThermoForgeGridHit Best;
    {
        UWorld* W = GetWorld();
        if (W)
        {
            bool FoundInContaining = false;
            for (TActorIterator<AThermoForgeVolume> It(W); It; ++It)
            {
                const AThermoForgeVolume* Vol = *It;
                if (!Vol || !Vol->BakedField) continue;
                if (!VolumeContainsPoint(Vol, WorldPos)) continue;

                FThermoForgeGridHit Hit;
                if (ComputeNearestInVolume(Vol, WorldPos, Hit))
                {
                    if (!FoundInContaining || Hit.DistanceSq < Best.DistanceSq)
                    {
                        Best = Hit; FoundInContaining = true;
                    }
                }
            }
            if (!FoundInContaining)
            {
                for (TActorIterator<AThermoForgeVolume> It(W); It; ++It)
                {
                    const AThermoForgeVolume* Vol = *It;
                    if (!Vol || !Vol->BakedField) continue;

                    FThermoForgeGridHit Hit;
                    if (ComputeNearestInVolume(Vol, WorldPos, Hit))
                    {
                        if (!Best.bFound || Hit.DistanceSq < Best.DistanceSq)
                            Best = Hit;
                    }
                }
            }
        }
    }

    float Sky = 0.f;
    if (Best.bFound && Best.Volume && Best.Volume->BakedField && Best.LinearIndex >= 0)
        Sky = FMath::Clamp(Best.Volume->BakedField->GetSkyViewByLinearIdx(Best.LinearIndex), 0.f, 1.f);

    const float AmbientC = S->GetAmbientCelsiusAt(bWinter, TimeHours, WorldPos.Z);
    const float SolarC   = S->SolarGainScaleC * Sky * (1.f - FMath::Clamp(WeatherAlpha01, 0.f, 1.f));

    return AmbientC + SolarC;
}

bool UThermoForgeSubsystem::FindBakedExtremeNear(const FVector& CenterWS, float RadiusCm, bool bHottest, FThermoForgeGridHit& OutHit, const FDateTime& QueryTimeUTC) const
{
    OutHit = FThermoForgeGridHit{};
    UWorld* W = GetWorld();
    const UThermoForgeProjectSettings* S = GetSettings();
    if (!W || !S) return false;

    // Choose the best volume (prefer containing).
    const AThermoForgeVolume* BestVol = nullptr;
    FThermoForgeGridHit Seed;
    {
        bool FoundInContaining = false;
        for (TActorIterator<AThermoForgeVolume> It(W); It; ++It)
        {
            const AThermoForgeVolume* Vol = *It;
            if (!Vol || !Vol->BakedField) continue;

            if (!FoundInContaining && VolumeContainsPoint(Vol, CenterWS))
            {
                FThermoForgeGridHit Hit;
                if (ComputeNearestInVolume(Vol, CenterWS, Hit))
                {
                    if (!BestVol || Hit.DistanceSq < Seed.DistanceSq)
                    {
                        BestVol = Vol; Seed = Hit; FoundInContaining = true;
                    }
                }
            }
        }
        if (!BestVol)
        {
            for (TActorIterator<AThermoForgeVolume> It(W); It; ++It)
            {
                const AThermoForgeVolume* Vol = *It;
                if (!Vol || !Vol->BakedField) continue;

                FThermoForgeGridHit Hit;
                if (ComputeNearestInVolume(Vol, CenterWS, Hit))
                {
                    if (!BestVol || Hit.DistanceSq < Seed.DistanceSq)
                    {
                        BestVol = Vol; Seed = Hit;
                    }
                }
            }
        }
    }
    if (!BestVol || !BestVol->BakedField) return false;

    const UThermoForgeFieldAsset* Field = BestVol->BakedField;
    const FIntVector D = Field->Dim;
    if (D.X<=0 || D.Y<=0 || D.Z<=0) return false;

    const float Cell = FMath::Max(1.f, Field->CellSizeCm);
    const FTransform Frame    = Field->GetGridFrame();
    const FTransform InvFrame = Frame.Inverse();

    // Center in grid-local units
    const FVector CenterLS = InvFrame.TransformPosition(CenterWS);
    const FVector CenterCell = CenterLS / Cell;

    const int32 cx = FMath::Clamp(FMath::FloorToInt(CenterCell.X + 0.5f), 0, D.X-1);
    const int32 cy = FMath::Clamp(FMath::FloorToInt(CenterCell.Y + 0.5f), 0, D.Y-1);
    const int32 cz = FMath::Clamp(FMath::FloorToInt(CenterCell.Z + 0.5f), 0, D.Z-1);

    const int32 R = FMath::Clamp(FMath::CeilToInt(RadiusCm / Cell), 0, 1024);

    auto Index = [&](int32 x,int32 y,int32 z){ return (z*D.Y + y)*D.X + x; };

    // Preview Knobs
    const bool  bWinter     = false;
    const float TimeHours   = 12.f;
    const float WeatherAlfa = 0.3f;

    float BestTemp = bHottest ? -FLT_MAX : +FLT_MAX;
    FIntVector BestIdx = Seed.GridIndex;
    FVector    BestPos = Seed.CellCenterWS;

    for (int32 z = FMath::Max(0, cz-R); z <= FMath::Min(D.Z-1, cz+R); ++z)
    for (int32 y = FMath::Max(0, cy-R); y <= FMath::Min(D.Y-1, cy+R); ++y)
    for (int32 x = FMath::Max(0, cx-R); x <= FMath::Min(D.X-1, cx+R); ++x)
    {
        // Keep spherical radius; skip if outside
        const FVector Delta(float(x-cx), float(y-cy), float(z-cz));
        if (Delta.SizeSquared() > float(R*R)) continue;

        const int32 lin = Index(x,y,z);

        // Baked-only composition: Ambient + Solar*Sky
        const float Sky = FMath::Clamp(Field->SkyView01.IsValidIndex(lin) ? Field->SkyView01[lin] : 0.f, 0.f, 1.f);

        const FVector CellCenterLS((x+0.5f)*Cell, (y+0.5f)*Cell, (z+0.5f)*Cell);
        const FVector CellCenterWS = Frame.TransformPosition(CellCenterLS);

        const float AmbientC = S->GetAmbientCelsiusAt(bWinter, TimeHours, CellCenterWS.Z);
        const float SolarC   = S->SolarGainScaleC * Sky * (1.f - WeatherAlfa);
        const float ThisC    = AmbientC + SolarC;

        const bool bBetter = bHottest ? (ThisC > BestTemp) : (ThisC < BestTemp);
        if (bBetter)
        {
            BestTemp = ThisC;
            BestIdx  = FIntVector(x,y,z);
            BestPos  = CellCenterWS;
        }
    }

    OutHit.bFound       = true;
    OutHit.Volume       = const_cast<AThermoForgeVolume*>(BestVol);
    OutHit.GridIndex    = BestIdx;
    OutHit.LinearIndex  = Index(BestIdx.X, BestIdx.Y, BestIdx.Z);
    OutHit.CellCenterWS = BestPos;
    OutHit.DistanceSq   = FVector::DistSquared(BestPos, CenterWS);
    OutHit.CellSizeCm   = Field->CellSizeCm;
    OutHit.QueryTimeUTC = QueryTimeUTC;
    OutHit.CurrentTempC = BestTemp;

    return true;
}
// bake per cell
void UThermoForgeSubsystem::TickBake()
{
    if (!BakeVolume.IsValid() || BakeTotalCells <= 0)
    {
        GetWorld()->GetTimerManager().ClearTimer(BakeTimerHandle);
        return;
    }

    const int32 Nx = BakeDim.X, Ny = BakeDim.Y, Nz = BakeDim.Z;
    const float RayLen = 100000.f;
    const int32 BatchSize = 500;

    for (int32 step=0; step<BatchSize && BakeProcessed < BakeTotalCells; ++step)
    {
        const int32 idx = BakeProcessed++;
        const int32 z = idx / (Nx * Ny);
        const int32 y = (idx / Nx) % Ny;
        const int32 x = idx % Nx;

        const FVector CenterLS(
            (Bake_ix0 + x + 0.5f) * BakeCell,
            (Bake_iy0 + y + 0.5f) * BakeCell,
            (Bake_iz0 + z + 0.5f) * BakeCell
        );
        const FVector P = BakeFrame.TransformPosition(CenterLS);

        // Sky openness
        float openness = 0.f;
        for (const FVector& d : BakeHemiDirs)
            openness += TraceAmbientRay01(P, d, RayLen);
        openness /= (float)BakeHemiDirs.Num();
        BakeSky[idx] = FMath::Clamp(openness, 0.f, 1.f);

        // Wall permeability
        float sumPerm = 0.f; int32 cnt = 0;
        const int32 nx[6] = { x-1, x+1, x,   x,   x,   x   };
        const int32 ny[6] = { y,   y,   y-1, y+1, y,   y   };
        const int32 nz[6] = { z,   z,   z,   z,   z-1, z+1 };

        for (int i=0;i<6;++i)
        {
            const int32 xx = nx[i], yy = ny[i], zz = nz[i];
            if (xx<0 || yy<0 || zz<0 || xx>=Nx || yy>=Ny || zz>=Nz) continue;

            const FVector NeighborLS(
                (Bake_ix0 + xx + 0.5f) * BakeCell,
                (Bake_iy0 + yy + 0.5f) * BakeCell,
                (Bake_iz0 + zz + 0.5f) * BakeCell
            );
            const FVector Q = BakeFrame.TransformPosition(NeighborLS);

            const float perm = OcclusionBetween(P, Q, BakeCell);
            sumPerm += FMath::Clamp(perm, 0.f, 1.f);
            ++cnt;
        }
        const float wallPerm = (cnt>0) ? (sumPerm / cnt) : 1.f;
        BakeWall[idx] = wallPerm;

        BakeIndoor[idx] = (1.f - BakeSky[idx]) * (1.f - BakeWall[idx]);
    }

    float Alpha = float(BakeProcessed) / float(BakeTotalCells);
    OnBakeProgress.Broadcast(Alpha);

    if (BakeProcessed >= BakeTotalCells)
    {
        GetWorld()->GetTimerManager().ClearTimer(BakeTimerHandle);
        // This a fallback to close progress
        OnBakeProgress.Broadcast(1.f);

#if WITH_EDITOR
        if (UThermoForgeFieldAsset* Saved = CreateAndSaveFieldAsset(
            BakeVolume.Get(), BakeDim, BakeCell,
            BakeFieldOriginWS, BakeFrame.Rotator(),
            BakeSky, BakeWall, BakeIndoor))
        {
            BakeVolume->Modify();
            BakeVolume->BakedField = Saved;
        #if WITH_EDITORONLY_DATA
            BakeVolume->GridPreviewISM->SetVisibility(true);
        #endif
            BakeVolume->BuildHeatPreviewFromField();
            BakeVolume->MarkPackageDirty();
        }
#endif
        BakeVolume = nullptr;
        StartNextBake(); 
    }
}

