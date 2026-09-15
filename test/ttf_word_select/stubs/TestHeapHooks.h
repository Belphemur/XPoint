// Host-test stub declarations for the fake heap tiers used by BookFontLoader tests.
#pragma once

#include <cstdint>

#include "HalMemory.h"

void testSetPsramHeap(HalMemory::HeapStats s);
void testSetInternalHeap(HalMemory::HeapStats s);
// Drive the ESP.getFreeHeap()/getMaxAllocHeap() stub that initBudget() reads.
void testSetFreeHeap(uint32_t freeHeap, uint32_t maxAllocHeap);