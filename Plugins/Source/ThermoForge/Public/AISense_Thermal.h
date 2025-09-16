#pragma once

#include "CoreMinimal.h"
#include "Perception/AISense.h"
#include "Perception/AISenseEvent.h"
#include "Perception/AIPerceptionTypes.h"
#include "AISense_Thermal.generated.h"

class UAISense_Thermal;
class UAIPerceptionComponent;
class UThermoForgeSubsystem;

/** Payload you post from gameplay/BP. */
USTRUCT(BlueprintType)
struct FAIThermalEvent
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Thermal")
    FVector Location = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Thermal")
    float TemperatureC = 0.f;

    /** Optional source actor (can be null). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Thermal")
    TObjectPtr<AActor> Instigator = nullptr;

    /** Optional tag to appear in AI Debug. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Thermal")
    FName Tag = NAME_None;
};

/** UObject event routed to this sense by the perception system. */
UCLASS()
class THERMOFORGE_API UAISenseEvent_Thermal : public UAISenseEvent
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Thermal")
    FAIThermalEvent Event;

    virtual FAISenseID GetSenseID() const override
    {
        return UAISense::GetSenseID<UAISense_Thermal>();
    }
};

/** Runtime implementation of the Thermal sense (UE 5.6-friendly). */
UCLASS(ClassGroup=AI)
class THERMOFORGE_API UAISense_Thermal : public UAISense
{
    GENERATED_BODY()

public:
    UAISense_Thermal();

    /** Post a thermal event (call on the server). */
    UFUNCTION(BlueprintCallable, Category="AI|Perception|Thermal")
    static void ReportThermalEvent(UObject* WorldContextObject, const FAIThermalEvent& Event);

    /** Perception passes UAISenseEvent_Thermal here. */
    virtual void RegisterWrappedEvent(UAISenseEvent& PerceptionEvent) override;

protected:
    /** Sense tick. Return next interval; 0 to tick next frame. */
    virtual float Update() override;

private:
    // We BIND to UAISense’s delegates in ctor (they're not virtual in 5.6).
    void HandleNewListener(const FPerceptionListener& NewListener);
    void HandleListenerRemoved(const FPerceptionListener& RemovedListener);
    void HandleListenerUpdate(const FPerceptionListener& UpdatedListener);

    void Enqueue(const FAIThermalEvent& Event);

    /** Events queued since last Update(). */
    UPROPERTY(Transient)
    TArray<FAIThermalEvent> RegisteredEvents;

    /** Active listener components having this sense enabled. */
    UPROPERTY(Transient)
    TArray<TWeakObjectPtr<UAIPerceptionComponent>> ListenerComps;
};
