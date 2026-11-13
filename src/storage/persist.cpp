#include "persist.h"

#include <Preferences.h>

static const char *NS = "cyd-jk-bms";
static const char *K_OUT = "out_t";

time_t persist_load_outage() {
  Preferences p;
  if (!p.begin(NS, true)) return 0;
  const time_t t = (time_t)p.getULong64(K_OUT, 0);
  p.end();
  return t;
}

void persist_save_outage(time_t t) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  if ((time_t)p.getULong64(K_OUT, 0) != t) p.putULong64(K_OUT, (uint64_t)t);
  p.end();
}
