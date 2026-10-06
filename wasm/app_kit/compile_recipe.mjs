// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
export function fileTree(flat) {
    const root = {};
    for (const [name, value] of Object.entries(flat)) {
        const parts = name.split('/'); let dir = root;
        for (const part of parts.slice(0, -1)) dir = dir[part] ??= {};
        dir[parts.at(-1)] = value;
    }
    return root;
}

export function substitute(args, values) {
    return args.flatMap(arg => {
        const key = arg.match(/^\{(\w+)\}$/)?.[1];
        if (key && Array.isArray(values[key])) return values[key];
        return arg.replace(/\{(\w+)\}/g, (_, token) => {
            if (!(token in values)) throw new Error('Unknown recipe token: ' + token);
            return values[token];
        });
    });
}

export async function compileApp(runClang, recipe, inputs, app) {
    if (recipe.recipe_format !== 1) throw new Error('Unsupported recipe format');
    if (typeof recipe.identifier_pattern !== 'string') throw new Error('Missing identifier pattern');
    const identifier = new RegExp(recipe.identifier_pattern);
    for (const value of [app.name, app.instance]) {
        // Require the whole value to match (the recipe's pattern may come from
        // other engines) before it enters a path or compiler argument.
        if (typeof value !== 'string' || identifier.exec(value)?.[0] !== value) throw new Error('Invalid app identifier');
    }
    const values = { name: app.name, instance: app.instance, prelude_output: recipe.prelude.output,
        objects: recipe.graphics.map(g => g.object), link_flags: recipe.link_flags };
    values.defines = substitute(recipe.defines, values);
    values.app_source = substitute([recipe.layout.source], values)[0];
    const sources = { ...inputs.sources,
        [substitute([recipe.layout.header], values)[0]]: app.header, [values.app_source]: app.source };
    const available = { sources, prelude: { [recipe.prelude.output]: inputs.prelude },
        ...Object.fromEntries(Object.entries(inputs.objects).map(([name, bytes]) => [name, { [name]: bytes }])) };
    const timings = {}, start = performance.now();
    let diagnostics = '';
    const quiet = { stdout: null, stderr: bytes => { if (bytes) diagnostics += new TextDecoder().decode(bytes); } };
    const command = async key => {
        const step = recipe.commands[key];
        const files = Object.assign({}, ...substitute(step.inputs, values).map(input => {
            if (!(input in available)) throw new Error('Unknown recipe input: ' + input);
            return available[input];
        }));
        const t = performance.now();
        try {
            return await runClang([recipe.driver, ...recipe.compile_flags,
                ...substitute(step.args, values)], fileTree(files), quiet);
        } catch (error) {
            error.compileStderr = diagnostics; throw error;
        } finally { timings[key + 'Ms'] = performance.now() - t; }
    };
    available['glue.o'] = { 'glue.o': (await command('glue'))['glue.o'] };
    available['app.o'] = { 'app.o': (await command('app'))['app.o'] };
    const wasm = (await command('link'))['app.wasm'];
    if (!(wasm instanceof Uint8Array)) throw new Error('No build output');
    return { wasm, diagnostics, timings: { ...timings, compileMs: performance.now() - start } };
}
