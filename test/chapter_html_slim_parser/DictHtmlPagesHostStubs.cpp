#include <Arduino.h>

// Strong override of Memory.h's weak host fallback: this suite stubs the ESP
// interface, so the heap gates consult the injectable stub heaps.
uint32_t poolMaxAllocForHostFallback() { return ESP.getMaxAllocHeap(); }
