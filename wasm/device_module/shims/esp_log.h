// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side esp_log shim: logging compiles out (printf would drag ~10KB of
// libc formatting into every module). Use cf_log() directly for guest debug.

#ifndef CF_DM_ESP_LOG_H
#define CF_DM_ESP_LOG_H

#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)

#endif  // CF_DM_ESP_LOG_H
