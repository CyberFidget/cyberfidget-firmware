# HAL import additivity checker

This tool enforces that the HAL import table stays additive forever, including WASI/env stubs. Removal, rename, C symbol or signature changes fail even when `CF_HAL_ABI` increases. Existing `since` levels cannot change. New rows must use `since` equal to head `CF_HAL_ABI` and above the base `CF_HAL_ABI` (a level the base already provides may be in released firmware, whose devices accept that level's stamp, so new imports always open a new level). Head `CF_HAL_ABI` cannot decrease and must equal the highest `since` in the table. Breaking changes require a new import name.

The table forms are `CF_ROW(since, name, ret, sig, policy, args)` and `CF_STUB(since, module, name, ret, sig, fn, policy, args)`. Tables predating levels are treated as level 1. The wrapper preprocesses the table into declarations for all modules; explicit declaration inputs may include `CF_IMPORT("module.name", since, "signature")`.

It uses only the Python 3 standard library. Compare the working tree with a Git ref:

```sh
python wasm/device_module/check_interface_additive.py --base-ref main
```

To compare explicit files, provide both import headers and both ABI headers:

```sh
python scripts/check_hal_imports_additive/check_hal_imports_additive.py \
  --base old/cf_hal_imports.h --base-abi old/cf_hal_abi.h \
  --head new/cf_hal_imports.h --head-abi new/cf_hal_abi.h
```

Run its self-contained tests from this directory:

```sh
python test_check_hal_imports_additive.py
```
