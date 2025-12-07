#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "PCGThermal_SampleClimate.generated.h"

UCLASS(BlueprintType, EditInlineNew, meta = (PCGNode))
class THERMOFORGE_API UPCGThermal_SampleClimateSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	virtual FName GetDefaultNodeName() const override
	{
		return TEXT("PCGThermal_SampleClimate");
	}

	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("ThermoForge", "PCGThermalSampleClimate", "Sample Climate");
	}

	virtual FText GetMenuCategory() const 
	{
		return NSLOCTEXT("ThermoForge", "ThermoForgeCategory", "ThermoForge");
	}

	virtual FString GetAdditionalTitleInformation() const override
	{
		return TEXT("ThermoForge: Sample Climate");
	}

protected:
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGThermal_SampleClimateElement : public IPCGElement
{
public:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
