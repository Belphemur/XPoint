#pragma once

// Host stand-in for the Arduino String type. The only uses compiled into this
// test (FsHelpers.h's inline String overloads) need c_str() and length().
#include <cstdint>
#include <string>

using String = std::string;
