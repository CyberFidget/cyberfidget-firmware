// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
__attribute__((import_module("cf"), import_name("made_up")))
extern void made_up(void);
void app_begin(void) { made_up(); }
void app_update(void) {}
void app_end(void) {}
void app_handle_button(int index, int event) { (void)index; (void)event; }
