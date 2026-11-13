#pragma once

#include <stddef.h>

void http_metrics_begin();
bool http_metrics_wifi_ok();
void http_metrics_ip(char *buf, size_t n);  // empty if not connected
