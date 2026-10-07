// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
#ifndef CF_PARAMS_H
#define CF_PARAMS_H
#define CF_EXPAND(...) __VA_ARGS__
#define CF_APPLY(mode, arg) CF_APPLY_I(mode, CF_EXPAND arg)
#define CF_APPLY_I(...) CF_APPLY_II(__VA_ARGS__)
#define CF_APPLY_II(mode, kind, type, name, bytes) CF_##mode##_##kind(type, name, bytes)
#define CF_PARAMS(mode, args) CF_PARAMS_I(mode, CF_EXPAND args)
#define CF_PARAMS_I(...) CF_PARAMS_II(__VA_ARGS__)
#define CF_PARAMS_II(mode, count, ...) CF_PARAMS_##count(mode, __VA_ARGS__)
#define CF_PARAMS_0(mode, ...) CF_##mode##_EMPTY
#define CF_PARAMS_1(mode, a) CF_APPLY(mode, a)
#define CF_PARAMS_2(mode, a, ...) CF_APPLY(mode, a) CF_PARAMS_1(mode, __VA_ARGS__)
#define CF_PARAMS_3(mode, a, ...) CF_APPLY(mode, a) CF_PARAMS_2(mode, __VA_ARGS__)
#define CF_PARAMS_4(mode, a, ...) CF_APPLY(mode, a) CF_PARAMS_3(mode, __VA_ARGS__)
#define CF_PARAMS_5(mode, a, ...) CF_APPLY(mode, a) CF_PARAMS_4(mode, __VA_ARGS__)
#define CF_PARAMS_6(mode, a, ...) CF_APPLY(mode, a) CF_PARAMS_5(mode, __VA_ARGS__)
#define CF_DECL_EMPTY void
#define CF_CALL_EMPTY
#define CF_CHECK_EMPTY
#define CF_UNPACK_EMPTY
#define CF_META_EMPTY
#define CF_DECL_A(type, name, bytes) type name
#define CF_DECL_B(type, name, bytes) , type name
#define CF_DECL_PA(type, name, bytes) CF_DECL_A(type, name, bytes)
#define CF_DECL_PB(type, name, bytes) CF_DECL_B(type, name, bytes)
#define CF_CALL_A(type, name, bytes) name
#define CF_CALL_B(type, name, bytes) , name
#define CF_CALL_PA(type, name, bytes) CF_CALL_A(type, name, bytes)
#define CF_CALL_PB(type, name, bytes) CF_CALL_B(type, name, bytes)
#endif
