// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Bring-your-own-key summary providers, called DIRECTLY from the browser.
// Ported from the App Builder's provider plumbing (cyberfidget_website
// builder.js) with one deliberate difference: the builder routes OpenAI
// through a cyberfidget.com proxy, but voice-derived text must never
// transit project servers, so here all three providers are direct calls.
//
// Key custody: keys live in this browser's localStorage, on the DEVICE's
// page origin (the same trust model as the website builder, but a
// different origin - the setup copy says so). Never sync, never upload.

const KEY_PREFIX = 'cfc_key_';
const PROVIDER_KEY = 'cfc_provider';
const MODEL_PREFIX = 'cfc_model_';

export const PROVIDERS = {
  anthropic: {
    label: 'Anthropic',
    consoleUrl: 'https://console.anthropic.com/settings/keys',
    models: [
      { id: 'claude-sonnet-4-6', label: 'Claude Sonnet 4.6 - balanced' },
      { id: 'claude-opus-4-7', label: 'Claude Opus 4.7 - smartest' },
      { id: 'claude-haiku-4-5-20251001', label: 'Claude Haiku 4.5 - fast' },
    ],
  },
  openai: {
    label: 'OpenAI',
    consoleUrl: 'https://platform.openai.com/api-keys',
    models: [
      { id: 'gpt-5.5', label: 'GPT-5.5 - flagship' },
      { id: 'gpt-5.4-mini', label: 'GPT-5.4 Mini - fast & budget' },
    ],
  },
  gemini: {
    label: 'Google',
    consoleUrl: 'https://aistudio.google.com/app/apikey',
    models: [
      { id: 'gemini-3.1-pro-preview', label: 'Gemini 3.1 Pro - flagship' },
      { id: 'gemini-3-flash-preview', label: 'Gemini 3 Flash - fast' },
      { id: 'gemini-2.5-flash', label: 'Gemini 2.5 Flash - stable budget' },
    ],
  },
};

export function getProvider() {
  const p = localStorage.getItem(PROVIDER_KEY);
  return PROVIDERS[p] ? p : 'anthropic';
}
export function setProvider(p) {
  if (PROVIDERS[p]) localStorage.setItem(PROVIDER_KEY, p);
}
export function getKey(p) {
  return localStorage.getItem(KEY_PREFIX + p) || '';
}
export function setKey(p, key) {
  if (key) localStorage.setItem(KEY_PREFIX + p, key);
  else localStorage.removeItem(KEY_PREFIX + p);
}
export function getModel(p) {
  const m = localStorage.getItem(MODEL_PREFIX + p);
  const list = PROVIDERS[p] ? PROVIDERS[p].models : [];
  return list.some((x) => x.id === m) ? m : (list[0] ? list[0].id : '');
}
export function setModel(p, m) {
  localStorage.setItem(MODEL_PREFIX + p, m);
}

// Provider-console link beside every key-entry / auth-error surface, so a
// dead key is always one tap from its fix.
export function consoleLink(p) {
  return PROVIDERS[p] ? PROVIDERS[p].consoleUrl : '';
}

class ProviderError extends Error {
  constructor(message, { authProblem = false } = {}) {
    super(message);
    this.authProblem = authProblem;
  }
}

async function callAnthropic(key, model, system, prompt, maxTokens) {
  const resp = await fetch('https://api.anthropic.com/v1/messages', {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      'x-api-key': key,
      'anthropic-version': '2023-06-01',
      // Browser-direct on purpose: the user's own key, straight to the
      // provider, no project server in the path.
      'anthropic-dangerous-direct-browser-access': 'true',
    },
    body: JSON.stringify({
      model,
      max_tokens: maxTokens,
      system,
      messages: [{ role: 'user', content: prompt }],
    }),
  });
  if (resp.status === 401) {
    throw new ProviderError('Anthropic did not accept that key.', { authProblem: true });
  }
  if (!resp.ok) {
    throw new ProviderError(`Anthropic error (${resp.status}). Try again in a moment.`);
  }
  const data = await resp.json();
  const text = (data.content || []).filter((c) => c.type === 'text').map((c) => c.text).join('');
  if (!text) throw new ProviderError('Anthropic sent back an empty reply.');
  return text;
}

async function callOpenAI(key, model, system, prompt, maxTokens) {
  const resp = await fetch('https://api.openai.com/v1/chat/completions', {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      'authorization': `Bearer ${key}`,
    },
    body: JSON.stringify({
      model,
      max_completion_tokens: maxTokens,
      messages: [
        { role: 'system', content: system },
        { role: 'user', content: prompt },
      ],
    }),
  });
  if (resp.status === 401) {
    throw new ProviderError('OpenAI did not accept that key.', { authProblem: true });
  }
  if (resp.status === 429) {
    throw new ProviderError('OpenAI says the account is over its limit - check billing.');
  }
  if (!resp.ok) {
    throw new ProviderError(`OpenAI error (${resp.status}). Try again in a moment.`);
  }
  const data = await resp.json();
  const text = data.choices && data.choices[0] && data.choices[0].message
    ? data.choices[0].message.content : '';
  if (!text) throw new ProviderError('OpenAI sent back an empty reply.');
  return text;
}

async function callGemini(key, model, system, prompt, maxTokens) {
  const url = `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(model)}:generateContent?key=${encodeURIComponent(key)}`;
  const resp = await fetch(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      systemInstruction: { parts: [{ text: system }] },
      contents: [{ role: 'user', parts: [{ text: prompt }] }],
      generationConfig: { maxOutputTokens: maxTokens },
    }),
  });
  if (resp.status === 400 || resp.status === 403) {
    throw new ProviderError('Google did not accept that key.', { authProblem: true });
  }
  if (!resp.ok) {
    throw new ProviderError(`Google error (${resp.status}). Try again in a moment.`);
  }
  const data = await resp.json();
  const cand = data.candidates && data.candidates[0];
  const text = cand && cand.content && cand.content.parts
    ? cand.content.parts.map((p) => p.text || '').join('') : '';
  if (!text) throw new ProviderError('Google sent back an empty reply.');
  return text;
}

// One entry point: generate `prompt` against the CONFIGURED provider with
// the user's stored key. Throws ProviderError with .authProblem set when
// the fix is "check your key" (callers link the provider console, per the
// project-wide invariant).
export async function generate(system, prompt, maxTokens = 1500) {
  const p = getProvider();
  const key = getKey(p);
  if (!key) {
    throw new ProviderError(`No ${PROVIDERS[p].label} key saved yet - add one in Setup.`, { authProblem: true });
  }
  const model = getModel(p);
  if (p === 'anthropic') return callAnthropic(key, model, system, prompt, maxTokens);
  if (p === 'openai') return callOpenAI(key, model, system, prompt, maxTokens);
  return callGemini(key, model, system, prompt, maxTokens);
}
