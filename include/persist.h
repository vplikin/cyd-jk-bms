#pragma once
#include <time.h>

// Small values kept in NVS flash so they survive reboots / power loss.
// Outage start as a UNIX time; 0 = no outage recorded.
time_t persist_load_outage();
void persist_save_outage(time_t t);
