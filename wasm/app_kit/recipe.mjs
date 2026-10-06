// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
// The format-1 recipe is data: consumers substitute tokens, then invoke commands.
export const recipe = {
    recipe_format: 1,
    identifier_pattern: '^[A-Za-z_][A-Za-z0-9_]*$',
    driver: 'clang++',
    compile_flags: ['--target=wasm32-wasip1', '-O2', '-fno-exceptions', '-fno-rtti',
        '-mcpu=mvp', '-mnontrapping-fptoint', '-mbulk-memory', '-msign-ext', '-mmutable-globals',
        '-Iwasm/device_module/shims', '-Iwasm/device_module', '-Iwasm/app', '-Ilib/CFGraphics/include'],
    defines: ['-DCF_WASM_APP_CUSTOM', '-DCF_CUSTOM_APP_HEADER="{name}.h"', '-DCUSTOM_APP_INSTANCE={instance}'],
    link_flags: ['-Wl,--initial-memory=262144', '-Wl,--max-memory=1048576',
        '-Wl,-z,stack-size=32768', '-mexec-model=reactor', '-Wl,--strip-all', '-Wl,--gc-sections'],
    force_include: 'wasm/device_module/shims/cf_custom_app_device.h',
    prelude: {
        source: 'prelude.h',
        expanded: 'prelude-expanded.h',
        output: 'prelude.pch',
        expand: { inputs: ['sources'], args: ['-E', '-dD', '{prelude_source}', '-o', '{prelude_expanded}'] },
        build: { inputs: ['sources'], args: ['-x', 'c++-header', '{prelude_expanded}', '-o', '{prelude_output}'] },
    },
    graphics: ['cf_gfx_sprite', 'cf_gfx_actor', 'cf_gfx_collision'].map(name => ({
        source: name + '.cpp', checkout_source: 'lib/CFGraphics/src/' + name + '.cpp', object: name + '.o',
    })),
    graphics_build: { inputs: ['sources', 'prelude'], args: ['-include-pch', '{prelude_output}', '-c', '{graphics_source}', '-o', '{graphics_object}'] },
    layout: {
        header: 'wasm/app/{name}.h', source: 'wasm/app/{name}.cpp',
        shims: 'wasm/device_module/shims/', interface: 'wasm/device_module/',
        graphics_headers: 'lib/CFGraphics/include/',
    },
    commands: {
        glue: { inputs: ['sources', 'prelude'], args: ['{defines}', '-include-pch', '{prelude_output}', '-c', 'wasm/device_module/shims/cf_app_glue.cpp', '-o', 'glue.o'] },
        app: { inputs: ['sources', 'prelude'], args: ['{defines}', '-include-pch', '{prelude_output}', '-c', '{app_source}', '-o', 'app.o'] },
        link: { inputs: ['glue.o', 'app.o', '{objects}'], args: ['-o', 'app.wasm', 'glue.o', 'app.o', '{objects}', '{link_flags}'] },
    },
};
