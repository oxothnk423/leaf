// TLS client stand-in.
//
// The emulator has no network path out (see HTTPClient.h), so this only needs to exist as a type
// HTTPClient::begin can accept.
#pragma once

#include <stddef.h>

class WiFiClientSecure {
 public:
  void setCACert(const char*) {}
  int lastError(char*, size_t) { return 0; }
};
