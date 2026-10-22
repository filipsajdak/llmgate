import { expect, test } from 'claude-code/testing'

import { compact, describeProvider, describeSpend, describeTotals, gatewayRoot, invocation, parseOverview, printable, describeRoute } from '../hooks/model'

const overview = JSON.stringify({
  schema_version: 1,
  roles: [
    { role: 'opus', via: 'config', providers: ['work'], upstream: 'opus-9' },
    { role: 'sonnet', via: 'session_remap', providers: ['codex'], upstream: 'gpt-6-sol' },
    { role: 'haiku', via: 'config', providers: [] },
  ],
  served: { provider: 'work', upstream: 'sonnet-9', source: 'config', fallback: true, switched: false },
  targets: [
    { id: 'claude-gpt-6-sol@codex', provider: 'codex', upstream: 'gpt-6-sol', label: 'gpt-6-sol (codex)', max_input_tokens: 272000 },
    { id: 'claude-sonnet-9@work', provider: 'work', upstream: 'sonnet-9', label: 'sonnet-9 (work)' },
  ],
  providers: [
    { name: 'work', attempts: 5, answered: 4, failures: 1, skipped: 0, circuit: 'closed', rate_limits: { 'anthropic-ratelimit-unified-5h-utilization': '0.42' } },
    { name: 'codex', attempts: 2, answered: 2, failures: 0, skipped: 0, circuit: 'open', rate_limits: {} },
  ],
  spend: {
    today: [
      { provider: 'work', model: 'sonnet-9', input: 1200000, output: 300000, cache_read: 5000000, cache_creation: 0, requests: 9, cost: 4.5 },
      { provider: 'codex', model: 'gpt-6-sol', input: 800, output: 90, cache_read: 0, cache_creation: 0, requests: 2 },
    ],
    month: [],
  },
  totals: { today: { input: 1200800, output: 300090, cache_read: 5000000, cache_creation: 0, requests: 11 }, month: { input: 9000000, output: 1, cache_read: 0, cache_creation: 0, requests: 40 }, budget: { state: 'warning', window: 'daily', used: 1500000, limit: 1800000, percent: 83 } },
})

test('an overview becomes routes, targets, providers, spend and totals', () => {
  const found = parseOverview('abc-123', overview)
  expect(found.sessionId).toBe('abc-123')
  expect(found.roles.map(route => route.role)).toEqual(['opus', 'sonnet', 'haiku'])
  expect(describeRoute(found.roles[1])).toBe('sonnet -> codex (gpt-6-sol)  via session_remap')
  expect(found.served?.fallback).toBe(true)
  expect(found.targets.map(item => item.id)).toEqual(['claude-gpt-6-sol@codex', 'claude-sonnet-9@work'])
  expect(found.providers.map(item => item.name)).toEqual(['work', 'codex'])
  expect(found.spendToday.length).toBe(2)
  expect(found.error).toBeUndefined()
})

test('an unreadable overview is an error, never a half-drawn state', () => {
  expect(parseOverview('s', '<html>').error).toBeDefined()
  expect(parseOverview('s', '{"schema_version":2}').error).toContain('version')
  expect(parseOverview('s', '{}').error).toBeDefined()
})

test('numbers, money, spend rows, providers and totals are written for a person', () => {
  const found = parseOverview('s', overview)
  expect(compact(999)).toBe('999')
  expect(compact(1250000)).toBe('1.3M')
  expect(compact(412345)).toBe('412k')
  expect(describeSpend(found.spendToday[0])).toBe('work  sonnet-9  1.2M in 300k out 5.0M cached  9 req  $4.50')
  expect(describeSpend(found.spendToday[1])).toBe('codex  gpt-6-sol  800 in 90 out  2 req  price unknown')
  expect(describeProvider(found.providers[0])).toBe('work  closed  5 tried, 4 answered, 1 failed  5h limit 42%')
  expect(describeProvider(found.providers[1])).toBe('codex  open  2 tried, 2 answered, 0 failed')
  expect(describeTotals(found)).toBe('today 1.5M tokens (11 req)  month 9.0M  budget warning 83% of daily 1.8M')
})

test('only a loopback http base is a gateway the mod may talk to', () => {
  expect(gatewayRoot('http://127.0.0.1:9482/anthropic')).toBe('http://127.0.0.1:9482')
  expect(gatewayRoot('http://localhost:9480')).toBe('http://localhost:9480')
  expect(gatewayRoot('https://api.anthropic.com')).toBeUndefined()
  expect(gatewayRoot('http://10.0.0.5:9480/anthropic')).toBeUndefined()
  expect(gatewayRoot(undefined)).toBeUndefined()
  expect(gatewayRoot('not a url')).toBeUndefined()
})

const targets = parseOverview('abc-123', overview).targets

test('a change becomes an argv list for the llmgate binary, scoped to the session by default', () => {
  expect(invocation('/bin/llmgate', '/c.jsonc', 'abc-123', targets, { role: 'sonnet', target: 'claude-gpt-6-sol@codex', scope: 'session' })).toEqual({
    argv: ['/bin/llmgate', '--config', '/c.jsonc', 'use', 'sonnet', 'claude-gpt-6-sol@codex', '--session', 'abc-123'],
  })
  expect(invocation('/bin/llmgate', undefined, 'abc-123', targets, { role: 'opus', target: 'claude-sonnet-9@work', scope: 'global' })).toEqual({
    argv: ['/bin/llmgate', 'use', 'opus', 'claude-sonnet-9@work'],
  })
  expect(invocation('/bin/llmgate', undefined, 'abc-123', targets, { role: 'haiku', clear: true, scope: 'session' })).toEqual({
    argv: ['/bin/llmgate', 'use', 'haiku', 'clear', '--session', 'abc-123'],
  })
})

test('a change that is not in the catalogue or has no binary or session is refused, never run', () => {
  expect('refused' in invocation('/bin/llmgate', undefined, 'abc', targets, { role: 'sonnet', target: 'claude-x@evil; rm -rf', scope: 'session' })).toBe(true)
  expect('refused' in invocation(undefined, undefined, 'abc', targets, { role: 'sonnet', clear: true, scope: 'session' })).toBe(true)
  expect('refused' in invocation('/bin/llmgate', undefined, '../x', targets, { role: 'sonnet', clear: true, scope: 'session' })).toBe(true)
  expect('refused' in invocation('/bin/llmgate', undefined, '', targets, { role: 'sonnet', clear: true, scope: 'global' })).toBe(false)
})

test('provider text never carries control bytes to the screen', () => {
  expect(printable('a\x1b[2Jb')).toBe('a?[2Jb')
  expect(printable('abcdef', 3)).toBe('abc')
})
