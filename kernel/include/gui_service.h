#ifndef PROSENJIT_GUI_SERVICE_H
#define PROSENJIT_GUI_SERVICE_H

#include <stdint.h>
#include "../include/abi/gui.h"

int pros_gui_validate_surface(const struct pros_gui_surface_request *req);
int pros_gui_validate_window(const struct pros_gui_window_request *req);
int pros_gui_validate_damage(const struct pros_gui_damage_request *req);
int pros_gui_validate_mode(const struct pros_gui_mode_request *req);
int pros_gui_validate_input(const struct pros_gui_input_event *event);
int pros_gui_selftest(void);

#endif
