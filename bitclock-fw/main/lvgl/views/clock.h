#include <stdbool.h>
#include <time.h>

typedef struct {
  time_t curtime;
  bool hour24;
  // Home Assistant status line; replaces the date when not empty.
  char status[25];
} lv_helper_view_mode_clock_data_t;
extern lv_helper_view_mode_clock_data_t lv_helper_view_mode_clock_data;

void lv_helper_clock_create();
void lv_helper_clock_update(lv_helper_view_mode_clock_data_t *data);
