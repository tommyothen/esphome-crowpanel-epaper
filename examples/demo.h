#pragma once
#include "esp_attr.h"

namespace demo {

// The count on the panel, kept through deep sleep.
inline int &count() {
  static RTC_DATA_ATTR int value = 0;
  return value;
}

enum Kind : int { NONE, PARTIAL, GREY, WAKE };

}  // namespace demo
