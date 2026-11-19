#pragma once
#include "Stats/Stats.h"
DECLARE_STATS_GROUP(TEXT("Flecs Network"), STATGROUP_FlecsNetwork, STATCAT_Advanced);
DECLARE_CYCLE_STAT_EXTERN(
	TEXT("Relevance selection"), STAT_FlecsNetworkSelect, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("State transfer"), STAT_FlecsNetworkState, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Relevant entities"), STAT_FlecsNetworkRelevant, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Selected entities"), STAT_FlecsNetworkSelected, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Deferred entities"), STAT_FlecsNetworkDeferred, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("State payload bytes sent"), STAT_FlecsNetworkStateSent, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("State payload bytes received"), STAT_FlecsNetworkStateReceived, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Command payload bytes sent"), STAT_FlecsNetworkCommandSent, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Command payload bytes received"), STAT_FlecsNetworkCommandReceived, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(
	TEXT("Manifest payload bytes sent"), STAT_FlecsNetworkManifestSent, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Manifest payload bytes received"), STAT_FlecsNetworkManifestReceived,
	STATGROUP_FlecsNetwork, FLECSNETWORK_API);
DECLARE_MEMORY_STAT_EXTERN(
	TEXT("Tracked channel buffers"), STAT_FlecsNetworkBuffers, STATGROUP_FlecsNetwork, FLECSNETWORK_API);
