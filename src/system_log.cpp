#include "system_log.h"

#include <stdarg.h>
#include <stdio.h>

namespace {
constexpr uint8_t kCapacity = 48;
constexpr size_t kMaxLine = 192;
String entries[kCapacity];
uint8_t count = 0;
uint8_t next = 0;

String escapeJson(const String& value) {
  String result;
  result.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') result += '\\';
    if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (static_cast<uint8_t>(c) >= 0x20) result += c;
  }
  return result;
}
}

namespace SystemLog {
void write(const char* message) {
  if (message == nullptr) return;
  Serial.println(message);
  String line(message);
  if (line.length() > kMaxLine) line.remove(kMaxLine);
  entries[next] = line;
  next = static_cast<uint8_t>((next + 1) % kCapacity);
  if (count < kCapacity) ++count;
}

void printf(const char* format, ...) {
  char buffer[kMaxLine + 1];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  write(buffer);
}

String json() {
  String result = "[";
  const uint8_t start = count == kCapacity ? next : 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (i != 0) result += ',';
    const uint8_t index = static_cast<uint8_t>((start + i) % kCapacity);
    result += "{\"line\":\"";
    result += escapeJson(entries[index]);
    result += "\"}";
  }
  result += ']';
  return result;
}

void clear() {
  for (auto& entry : entries) entry = String();
  count = 0;
  next = 0;
}
}
