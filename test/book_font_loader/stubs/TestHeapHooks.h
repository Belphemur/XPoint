// Host-test stub declarations for the fake heap tiers used by BookFontLoader tests.
#pragma once

#include "HalMemory.h"

void testSetPsramHeap(HalMemory::HeapStats s);
void testSetInternalHeap(HalMemory::HeapStats s);