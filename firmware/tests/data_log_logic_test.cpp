#include <cstdio>
#include <cstring>

#include "../src/data_log_logic.h"

int main() {
  char output[64] = {};
  const char* secret = "{\"token\":\"super-secret\"";
  formatMalformedJsonLog(std::strlen(secret), output, sizeof(output));
  if (std::strstr(output, "super-secret") || std::strstr(output, "token")) return 1;
  if (!std::strstr(output, "json malformed") || !std::strstr(output, "len=23")) return 2;
  return 0;
}
