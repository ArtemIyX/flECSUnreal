#include "Settings/FlecsDeveloperSettings.h"

UFlecsDeveloperSettings::UFlecsDeveloperSettings()
{
	DistanceUpdateRates.Emplace(5000.0f, 30.0f, FColor::Green);
	DistanceUpdateRates.Emplace(15000.0f, 15.0f, FColor::Yellow);
	DistanceUpdateRates.Emplace(30000.0f, 7.5f, FColor::Orange);
	DistanceUpdateRates.Emplace(60000.0f, 3.75f, FColor::Red);
}
