#include "AISense_Thermal.h"

#include "AIController.h"
#include "AISenseConfig_Thermal.h"
#include "Perception/AIPerceptionSystem.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionTypes.h"  
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/Package.h"             
#include "DrawDebugHelpers.h"
#include "ThermoForgeSubsystem.h"

UAISense_Thermal::UAISense_Thermal()
{
    NotifyType = EAISenseNotifyType::OnPerceptionChange;

    OnNewListenerDelegate     = FOnPerceptionListenerUpdateDelegate::CreateUObject(this, &UAISense_Thermal::HandleNewListener);
    OnListenerRemovedDelegate = FOnPerceptionListenerUpdateDelegate::CreateUObject(this, &UAISense_Thermal::HandleListenerRemoved);
    OnListenerUpdateDelegate  = FOnPerceptionListenerUpdateDelegate::CreateUObject(this, &UAISense_Thermal::HandleListenerUpdate);

    UE_LOG(LogTemp, Log, TEXT("[ThermoForge] Constructed. SenseID=%d"), GetSenseID().Index);
}

void UAISense_Thermal::ReportThermalEvent(UObject* WorldContextObject, const FAIThermalEvent& Event)
{
    if (UAIPerceptionSystem* Sys = UAIPerceptionSystem::GetCurrent(WorldContextObject))
    {
        UAISenseEvent_Thermal* NewEvt = NewObject<UAISenseEvent_Thermal>(GetTransientPackage());
        NewEvt->Event = Event;
        Sys->ReportEvent(NewEvt);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[ThermoForge] UAIPerceptionSystem::GetCurrent returned null"));
    }
}

void UAISense_Thermal::RegisterWrappedEvent(UAISenseEvent& PerceptionEvent)
{
    if (UAISenseEvent_Thermal* Evt = Cast<UAISenseEvent_Thermal>(&PerceptionEvent))
    {
        Enqueue(Evt->Event);
    }
}

void UAISense_Thermal::HandleNewListener(const FPerceptionListener& NewListener)
{
    if (UAIPerceptionComponent* Comp = NewListener.Listener.Get())
    {
        ListenerComps.AddUnique(Comp);
        UE_LOG(LogTemp, Log, TEXT("[ThermoForge] New listener: %s"), *GetNameSafe(Comp));
    }
}

void UAISense_Thermal::HandleListenerRemoved(const FPerceptionListener& RemovedListener)
{
    if (UAIPerceptionComponent* Comp = RemovedListener.Listener.Get())
    {
        ListenerComps.RemoveAll([Comp](const TWeakObjectPtr<UAIPerceptionComponent>& X){ return X.Get() == Comp; });
        UE_LOG(LogTemp, Log, TEXT("[ThermoForge] Listener removed: %s"), *GetNameSafe(Comp));
    }
}

void UAISense_Thermal::HandleListenerUpdate(const FPerceptionListener& UpdatedListener)
{
    if (UAIPerceptionComponent* Comp = UpdatedListener.Listener.Get())
    {
        if (!ListenerComps.Contains(Comp))
        {
            ListenerComps.Add(Comp);
            UE_LOG(LogTemp, Log, TEXT("[ThermoForge] Listener updated/added: %s"), *GetNameSafe(Comp));
        }
    }
}

void UAISense_Thermal::Enqueue(const FAIThermalEvent& Event)
{
    RegisteredEvents.Add(Event);
    RequestImmediateUpdate();
}

// ---------- Helpers----------
static bool HasLineOfSightMulti(UWorld& World,
                                const UAISenseConfig_Thermal& Cfg,
                                const FVector& From,
                                const FVector& To,
                                const TArray<const AActor*>& ExtraIgnored)
{
    if (!Cfg.bUseLineOfSight) return true;

    const int32 Steps = FMath::Max(1, Cfg.LoSSteps);
    const FVector Delta = (To - From) / float(Steps);

    FCollisionQueryParams Q(TEXT("ThermalLOS"), /*bTraceComplex*/ false);
    for (const AActor* Ign : ExtraIgnored)
    {
        if (IsValid(Ign)) { Q.AddIgnoredActor(Ign); }
    }

    FHitResult Hit;
    FVector A = From;
    for (int32 i = 0; i < Steps; ++i)
    {
        const FVector B = (i + 1 == Steps) ? To : (A + Delta);
        if (World.LineTraceSingleByChannel(Hit, A, B, ECC_Visibility, Q))
            return false;
        A = B;
    }
    return true;
}


static AActor* TF_ResolveListenerActor(UAIPerceptionComponent* Comp, FVector& OutLoc, FVector& OutForward)
{
    OutLoc = FVector::ZeroVector;
    OutForward = FVector::ForwardVector;

    if (!Comp) return nullptr;
    AActor* Owner = Comp->GetOwner();

    // If component is on an AIController, prefer its pawn
    if (AAIController* C = Cast<AAIController>(Owner))
        if (APawn* P = C->GetPawn())
        {
            OutLoc     = P->GetActorLocation();
            OutForward = P->GetActorForwardVector();
            return P;
        }

    // Else use the owner directly (component on Pawn/Character, etc.)
    if (Owner)
    {
        OutLoc     = Owner->GetActorLocation();
        OutForward = Owner->GetActorForwardVector();
    }
    return Owner;
}

static float ChooseCadence(const UAISenseConfig_Thermal& Cfg, float DistSq)
{
    if (DistSq <= FMath::Square(Cfg.NearRange)) return Cfg.NearUpdate;
    if (DistSq <= FMath::Square(Cfg.MidRange))  return Cfg.MidUpdate;
    return Cfg.FarUpdate;
}

// 2D ring sampling around a listener (uses NearRange/MidRange)
static bool ProbeThermoRing(UWorld& World,
                            UThermoForgeSubsystem* Thermo,
                            const UAISenseConfig_Thermal& Cfg,
                            const FVector& ListenerLoc,
                            float& OutBestTempC,
                            FVector& OutBestLoc)
{
    if (!Thermo) return false;

    const float R1 = FMath::Max(0.f, Cfg.NearRange);
    const float R2 = FMath::Max(R1, Cfg.MidRange > 0.f ? Cfg.MidRange : R1);

    // Choose number of samples per ring; keep cheap
    const int32 N1 = 16;
    const int32 N2 = 24;

    bool bFound = false;
    float BestT = -FLT_MAX;
    FVector BestP = ListenerLoc;

    auto SampleRing = [&](float Radius, int32 Count)
    {
        if (Radius <= 1.f || Count <= 0) return;
        for (int32 i = 0; i < Count; ++i)
        {
            const float Ang = (2.f * PI) * (float(i) / float(Count));
            const FVector2D D2(FMath::Cos(Ang), FMath::Sin(Ang));
            const FVector P = ListenerLoc + FVector(D2.X * Radius, D2.Y * Radius, 0.f);

            // LOS
            if (!HasLineOfSightMulti(World, Cfg, ListenerLoc, P, {}))
                continue;

            const float T = Thermo->ComputeCurrentTemperatureAt(P, /*bWinter=*/false, /*TimeHours=*/12.f, /*WeatherAlpha01=*/0.3f);
            if (T > BestT)
            {
                BestT = T;
                BestP = P;
                bFound = true;
            }
            if (Cfg.bDirectional)
            {
                const AActor* Owner = World.GetFirstPlayerController() ? nullptr : nullptr; 
                const FVector Forward = Owner ? Owner->GetActorForwardVector() : FVector::ForwardVector;
                const FVector ToP     = (P - ListenerLoc).GetSafeNormal();
                const float   CosHalf = FMath::Cos(FMath::DegreesToRadians(Cfg.FOV));
                if (FVector::DotProduct(Forward, ToP) < CosHalf)
                    return; // skip sample
            }
        }
    };

    SampleRing(R1, N1);
    SampleRing(R2, N2);

    OutBestTempC = BestT;
    OutBestLoc   = BestP;
    return bFound;
}

float UAISense_Thermal::Update()
{
    if (RegisteredEvents.Num() == 0 && ListenerComps.Num() == 0)
        return 0.25f; // idle cadence

    UWorld* World = GetWorld();
    if (!World) { RegisteredEvents.Reset(); return 0.25f; }

    UThermoForgeSubsystem* Thermo = World->GetSubsystem<UThermoForgeSubsystem>();

    // ----- AMBIENT GRID PROBE -----
    float NextInterval = 0.33f; // default fallback

    for (int32 i = ListenerComps.Num() - 1; i >= 0; --i)
    {
        UAIPerceptionComponent* Comp = ListenerComps[i].Get();
        if (!Comp) { ListenerComps.RemoveAtSwap(i); continue; }
        if (!Comp->IsSenseEnabled(UAISense_Thermal::StaticClass())) continue;

        const UAISenseConfig_Thermal* Cfg = Cast<UAISenseConfig_Thermal>(Comp->GetSenseConfig(GetSenseID()));
        if (!Cfg) continue;

        AActor* Owner = Comp->GetOwner();
        if (!Owner) continue;

        FVector L, Fwd;
        AActor* ListenerActor = TF_ResolveListenerActor(Comp, L, Fwd);
        if (!ListenerActor) continue;
        
        // Probe the grid around the listener and pick the hottest visible point
        float BestTempC = -FLT_MAX;
        FVector BestLoc = L;
        const bool bAny = ProbeThermoRing(*World, Thermo, *Cfg, L, BestTempC, BestLoc);

        if (bAny)
        {
            const float ThresholdC = Cfg->HeatThresholdC;
            const float HotRefC    = FMath::Max(ThresholdC + 20.f, ThresholdC + 1.f);
            const float Strength   = FMath::Clamp((BestTempC - ThresholdC) / (HotRefC - ThresholdC), 0.f, 1.f);

            if (Strength > 0.f)
            {
                // OMNIDIRECTIONAL (no FOV)
                FAIStimulus Stim(*this, Strength, BestLoc, L, FAIStimulus::FResult::SensingSucceeded, FName(TEXT("ThermalAmbient")));
                Stim.SetExpirationAge(3.f);

                // we just care about a hot spot, not a specific actor
                Comp->RegisterStimulus(Owner, Stim);
                Comp->RequestStimuliListenerUpdate();

            }
        }

        // Ambient cadence per listener
        NextInterval = FMath::Min(NextInterval, Cfg->MidUpdate);

    }

    // ----- PROCESS QUEUED ONE-SHOT EVENTS -----
    for (const FAIThermalEvent& E : RegisteredEvents)
    {
        for (int32 i = ListenerComps.Num() - 1; i >= 0; --i)
        {
            UAIPerceptionComponent* Comp = ListenerComps[i].Get();
            if (!Comp) { ListenerComps.RemoveAtSwap(i); continue; }
            if (!Comp->IsSenseEnabled(UAISense_Thermal::StaticClass())) continue;

            const UAISenseConfig_Thermal* Cfg = Cast<UAISenseConfig_Thermal>(Comp->GetSenseConfig(GetSenseID()));
            if (!Cfg) continue;

            AActor* Owner = Comp->GetOwner();
            if (!Owner) continue;

            // Emitter tag filter
            if (!Cfg->EmitterActorTag.IsNone() && E.Instigator)
            {
                if (!E.Instigator->ActorHasTag(Cfg->EmitterActorTag))
                    continue;
            }

            FVector OwnerLoc, Fwd;
            AActor* ListenerActor = TF_ResolveListenerActor(Comp, OwnerLoc, Fwd);
            if (!ListenerActor) continue;

            if (!HasLineOfSightMulti(*World, *Cfg, OwnerLoc, E.Location, { Owner, E.Instigator }))
                continue;

            // If grid has even hotter value at the event spot, prefer that
            float EventTempC = E.TemperatureC;
            if (Thermo)
            {
                const float GridC = Thermo->ComputeCurrentTemperatureAt(E.Location, /*bWinter=*/false, /*TimeHours=*/12.f, /*WeatherAlpha01=*/0.3f);
                EventTempC = FMath::Max(EventTempC, GridC);
            }

            const float ThresholdC = Cfg->HeatThresholdC;
            const float HotRefC    = FMath::Max(ThresholdC + 20.f, ThresholdC + 1.f);
            const float Strength   = FMath::Clamp((EventTempC - ThresholdC) / (HotRefC - ThresholdC), 0.f, 1.f);

            if (Strength <= 0.f) continue;

            FAIStimulus Stim(*this, Strength, E.Location, OwnerLoc,
                             FAIStimulus::FResult::SensingSucceeded,
                             E.Tag.IsNone() ? FName(TEXT("Thermal")) : E.Tag);
            Stim.SetExpirationAge(3.f);

            AActor* Source = E.Instigator;
            Comp->RegisterStimulus(Source, Stim);
            Comp->RequestStimuliListenerUpdate();

            NextInterval = FMath::Min(NextInterval, ChooseCadence(*Cfg, Cfg->MidUpdate));
        }
    }

    RegisteredEvents.Reset();
    return NextInterval;
}
