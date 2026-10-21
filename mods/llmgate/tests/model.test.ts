import { expect, test } from 'claude-code/testing'

import { describeRoute, gatewayRoot, invocation, parseRoute, parseServed, parseTargets, printable } from '../hooks/model'

const models = JSON.stringify({
  data: [
    { type: 'model', id: 'opus', display_name: 'opus' },
    { type: 'model', id: 'claude-gpt-6-sol@codex', display_name: 'gpt-6-sol (codex)', description: 'served by the provider codex' },
    { type: 'model', id: 'claude-sonnet-9@work', display_name: 'sonnet-9 (work)', description: 'served by the provider work' },
  ],
})

test('targets are the provider-qualified entries, aliases are left out', () => {
  const found = parseTargets(models)
  expect(found.map(item => item.id)).toEqual(['claude-gpt-6-sol@codex', 'claude-sonnet-9@work'])
  expect(found[0].provider).toBe('codex')
  expect(found[0].label).toBe('gpt-6-sol (codex)')
})

test('an unreadable or foreign model list gives no targets', () => {
  expect(parseTargets('<html>')).toEqual([])
  expect(parseTargets('{"data":"x"}')).toEqual([])
  expect(parseTargets('{"data":[{"id":"x@y","display_name":"a\\u001b[31m"}]}')[0].label).toBe('a?[31m')
})

test('a route reply becomes a role route', () => {
  const route = parseRoute('sonnet', '{"role":"sonnet","via":"session_remap","providers":["codex"],"upstream":"gpt-6-sol"}')
  expect(route).toEqual({ role: 'sonnet', via: 'session_remap', providers: ['codex'], upstream: 'gpt-6-sol' })
  expect(describeRoute(route)).toBe('sonnet -> codex (gpt-6-sol)  via session_remap')
  expect(parseRoute('opus', 'nope')).toEqual({ role: 'opus', via: 'unknown', providers: [] })
})

test('the served view is empty until the session has had a reply', () => {
  expect(parseServed('{}')).toBeUndefined()
  expect(parseServed('{"provider":"work","upstream":"m","source":"config","fallback":true,"switched":false}')).toEqual({
    provider: 'work',
    upstream: 'm',
    source: 'config',
    fallback: true,
    switched: false,
  })
})

test('only a loopback http base is a gateway the mod may talk to', () => {
  expect(gatewayRoot('http://127.0.0.1:9482/anthropic')).toBe('http://127.0.0.1:9482')
  expect(gatewayRoot('http://localhost:9480')).toBe('http://localhost:9480')
  expect(gatewayRoot('https://api.anthropic.com')).toBeUndefined()
  expect(gatewayRoot('http://10.0.0.5:9480/anthropic')).toBeUndefined()
  expect(gatewayRoot(undefined)).toBeUndefined()
  expect(gatewayRoot('not a url')).toBeUndefined()
})

const targets = parseTargets(models)

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
