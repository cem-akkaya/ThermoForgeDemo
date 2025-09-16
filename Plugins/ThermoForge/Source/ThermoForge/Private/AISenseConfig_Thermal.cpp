#include "AISenseConfig_Thermal.h"
#include "AISense_Thermal.h"

UAISenseConfig_Thermal::UAISenseConfig_Thermal()
{
	DebugColor = FColor(255, 120, 0); // orange for AI Debug
}

TSubclassOf<UAISense> UAISenseConfig_Thermal::GetSenseImplementation() const
{
	return UAISense_Thermal::StaticClass();
}
