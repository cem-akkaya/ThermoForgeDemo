#include "PCGThermal_SampleClimate.h"
#include "ThermoForgeSubsystem.h"

#include "PCGContext.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"

// Debug color mapping for climate
static FVector4 ClimateToDebugColor(EThermoClimateType C)
{
	switch (C)
	{
		case EThermoClimateType::Arctic:     return FVector4(0.0f, 0.25f, 1.0f, 1.0f);
		case EThermoClimateType::Cold:       return FVector4(0.0f, 0.75f, 1.0f, 1.0f);
		case EThermoClimateType::Temperate:  return FVector4(0.0f, 1.0f, 0.0f, 1.0f);
		case EThermoClimateType::Warm:       return FVector4(1.0f, 1.0f, 0.0f, 1.0f);
		case EThermoClimateType::Tropical:   return FVector4(1.0f, 0.5f, 0.0f, 1.0f);
		case EThermoClimateType::Desert:     return FVector4(1.0f, 0.0f, 0.0f, 1.0f);
		default:                             return FVector4(1, 1, 1, 1);
	}
}

FPCGElementPtr UPCGThermal_SampleClimateSettings::CreateElement() const
{
	return MakeShared<FPCGThermal_SampleClimateElement>();
}

bool FPCGThermal_SampleClimateElement::ExecuteInternal(FPCGContext* Context) const
{
	if (!Context)
		return true;

	// Resolve world
	UObject* ExecObj = Context->ExecutionSource.GetObject();
	UWorld* World = ExecObj ? ExecObj->GetWorld() : nullptr;

	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SampleClimate] No valid world"));
		return true;
	}

	UThermoForgeSubsystem* Subsystem = World->GetSubsystem<UThermoForgeSubsystem>();
	if (!Subsystem)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SampleClimate] ThermoForge subsystem missing"));
		return true;
	}

	// Get ALL spatial inputs 
	const TArray<FPCGTaggedData>& Inputs = Context->InputData.GetAllSpatialInputs();
	if (Inputs.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[SampleClimate] No spatial input"));
		return true;
	}

	const UPCGPointData* InPointData = Cast<UPCGPointData>(Inputs[0].Data);
	if (!InPointData)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SampleClimate] Input is not point data"));
		return true;
	}

	// Duplicate for output
	UPCGPointData* OutPointData = CastChecked<UPCGPointData>(InPointData->DuplicateData(Context));
	TArray<FPCGPoint>& Points = OutPointData->GetMutablePoints();

	UPCGMetadata* Metadata = OutPointData->Metadata;
	if (!Metadata)
	{
		UE_LOG(LogTemp, Error, TEXT("[SampleClimate] Missing metadata"));
		return true;
	}

	// Create/find metadata attribute
	const FName ClimateAttr = TEXT("ClimateType");
	FPCGMetadataAttribute<int32>* Attr =
		Metadata->FindOrCreateAttribute<int32>(ClimateAttr, 0);

	if (!Attr)
	{
		UE_LOG(LogTemp, Error, TEXT("[SampleClimate] Failed to create ClimateType attribute"));
		return true;
	}

	// Process points
	for (FPCGPoint& Point : Points)
	{
		// Ensure metadata entry exists 
		if (Point.MetadataEntry == PCGInvalidEntryKey)
		{
			Point.MetadataEntry = Metadata->AddEntry();
		}

		const FVector Pos = Point.Transform.GetLocation();
		const EThermoClimateType Climate = Subsystem->GetClimateTypeAtPoint(Pos);

		// Write metadata (safe now)
		Attr->SetValue(Point.MetadataEntry, (int32)Climate);

		// Debug color so PCG displays colored cubes
		Point.Color = ClimateToDebugColor(Climate);
	}

	// Output result
	FPCGTaggedData& Output = Context->OutputData.TaggedData.Add_GetRef(Inputs[0]);
	Output.Data = OutPointData;

	return true;
}
