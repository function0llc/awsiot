#pragma once

#include "esp_err.h"

esp_err_t time_sync_wait_for_time(int max_wait_seconds);
