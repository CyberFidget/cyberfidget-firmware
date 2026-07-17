# HAL import additivity checker

This tool enforces that the HAL import table stays additive within an ABI major version. It reports imports that were removed or renamed, whose C symbol changed, or whose signature changed. A change is allowed when `CF_HAL_ABI` changes to a new major.

It uses only the Python 3 standard library. Compare the working tree with a Git ref:

```sh
python scripts/check_hal_imports_additive/check_hal_imports_additive.py --base-ref origin/main
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
