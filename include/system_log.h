#pragma once

#include <Arduino.h>

namespace SystemLog {
void write(const char* message);
void printf(const char* format, ...);
String json();
void clear();
}
