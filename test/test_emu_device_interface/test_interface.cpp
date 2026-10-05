// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
#include <unity.h>
#include <string>
#include <type_traits>
#include "../../lib/WasmAppRuntime/WasmHostFunctions.h"

template<class T> static char wasmType() {
    if (std::is_same<T, void>::value) return 'v';
    if (std::is_same<T, float>::value) return 'f';
    if (std::is_same<T, double>::value) return 'F';
    if (std::is_pointer<T>::value || sizeof(T) <= 4) return 'i';
    return 'I';
}
template<> char wasmType<void>() { return 'v'; }

static void test_table_signatures_match_c_types() {
#define CF_ROW(name, ret, sig, policy, args) { \
    const auto params = std::string() CF_PARAMS(TYPES_TAIL, args); \
    const auto actual = std::string(1, wasmType<ret>()) + "(" + params + ")"; \
    TEST_ASSERT_EQUAL_STRING_MESSAGE(sig, actual.c_str(), #name); }
#define CF_TYPES_TAIL_EMPTY
#define CF_TYPES_TAIL_A(type, name, bytes) + std::string(1, wasmType<type>())
#define CF_TYPES_TAIL_B(type, name, bytes) CF_TYPES_TAIL_A(type, name, bytes)
#define CF_TYPES_TAIL_PA(type, name, bytes) CF_TYPES_TAIL_A(type, name, bytes)
#define CF_TYPES_TAIL_PB(type, name, bytes) CF_TYPES_TAIL_A(type, name, bytes)
#define CF_STUB(module, name, ret, sig, fn, policy, args) CF_ROW(name, ret, sig, policy, args)
#include "../../wasm/device_module/cf_imports.def"
#undef CF_STUB
#undef CF_ROW
}
static void test_xbm_limits_and_odd_width() {
    TEST_ASSERT_EQUAL_UINT32(32, cf_host::xbmByteLength(15, 16));
    TEST_ASSERT_EQUAL_UINT32(1024, cf_host::xbmByteLength(128, 64));
    TEST_ASSERT_EQUAL_UINT32(0, cf_host::xbmByteLength(129, 64));
    TEST_ASSERT_EQUAL_UINT32(0, cf_host::xbmByteLength(128, 65));
    TEST_ASSERT_EQUAL_UINT32(0, cf_host::xbmByteLength(-1, 64));
    TEST_ASSERT_EQUAL_UINT32(0, cf_host::xbmByteLength(128, 0));
}
static void test_sequence_count_clamps_before_memory_validation() {
    TEST_ASSERT_EQUAL_INT(0, cf_host::sequenceCount(-1));
    TEST_ASSERT_EQUAL_INT(0, cf_host::sequenceCount(INT32_MIN));
    TEST_ASSERT_EQUAL_INT(3, cf_host::sequenceCount(3));
    TEST_ASSERT_EQUAL_INT(64, cf_host::sequenceCount(65));
    TEST_ASSERT_EQUAL_INT(64, cf_host::sequenceCount(INT32_MAX));
}
void setUp() {}
void tearDown() {}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_table_signatures_match_c_types);
    RUN_TEST(test_xbm_limits_and_odd_width);
    RUN_TEST(test_sequence_count_clamps_before_memory_validation);
    return UNITY_END();
}
