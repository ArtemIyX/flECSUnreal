#pragma once
#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("Flecs Simulation"), STATGROUP_Flecs, STATCAT_Advanced);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Worker step"), STAT_FlecsWorkerStep, STATGROUP_Flecs, FLECS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Flecs world progress"), STAT_FlecsWorldProgress, STATGROUP_Flecs, FLECS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Publication extraction"), STAT_FlecsPublication, STATGROUP_Flecs, FLECS_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Commands applied"), STAT_FlecsCommands, STATGROUP_Flecs, FLECS_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Publication items"), STAT_FlecsPublicationItems, STATGROUP_Flecs, FLECS_API);
DECLARE_MEMORY_STAT_EXTERN(TEXT("Sampled Flecs world memory"), STAT_FlecsWorldMemory, STATGROUP_Flecs, FLECS_API);
DECLARE_MEMORY_STAT_EXTERN(TEXT("Copied publication allocation"), STAT_FlecsPublicationMemory, STATGROUP_Flecs, FLECS_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Queue depth samples"), STAT_FlecsQueueDepth, STATGROUP_Flecs, FLECS_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Missed deadlines"), STAT_FlecsMissedDeadlines, STATGROUP_Flecs, FLECS_API);
DECLARE_FLOAT_COUNTER_STAT_EXTERN(TEXT("Flecs system time (ms)"), STAT_FlecsSystemMilliseconds, STATGROUP_Flecs, FLECS_API);
