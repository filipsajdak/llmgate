import { expect, test } from 'claude-code/testing'

import { agentTable, changes, compact, describeTotals, providerTable, routeTable, spendTable, table, unpricedNote, gatewayRoot, invocation, parseOverview, parseCommand, printable, printableBlock, resolveTarget, statusText, describeTargets } from '../hooks/model'

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
  expect(routeTable(found.roles)).toEqual([
    'role    provider  model      via',
    '------  --------  ---------  -------------',
    'opus    work      opus-9     config',
    'sonnet  codex     gpt-6-sol  session_remap',
    'haiku   none      -          config',
  ])
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
  expect(spendTable(found.spendToday)).toEqual([
    'provider  model        in   out  cached  req   cost',
    '--------  ---------  ----  ----  ------  ---  -----',
    'work      sonnet-9   1.2M  300k    5.0M    9  $4.50',
    'codex     gpt-6-sol   800    90       -    2      -',
  ])
  expect(unpricedNote(found.spendToday)).toContain('add the model to pricing')
  expect(unpricedNote([found.spendToday[0]])).toBeUndefined()
  expect(providerTable(found.providers)).toEqual([
    'provider  labels  auth     state  circuit  tried  answered  failed  limits',
    '--------  ------  -------  -----  -------  -----  --------  ------  ------------',
    'work      -       unknown  ready  closed       5         4       1  5h limit 42%',
    'codex     -       unknown  ready  open         2         2       0  -',
  ])
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

const found = parseOverview('s1', overview)

test('parseCommand reads the subcommands and refuses the rest with the list of what exists', () => {
  expect(parseCommand('')).toEqual({ kind: 'pane' })
  expect(parseCommand('  pane ')).toEqual({ kind: 'pane' })
  expect(parseCommand('status')).toEqual({ kind: 'status' })
  expect(parseCommand('setup')).toEqual({ kind: 'setup' })
  expect(parseCommand('recipes')).toEqual({ kind: 'recipes' })
  expect(parseCommand('agents')).toEqual({ kind: 'agents' })
  expect(parseCommand('scorecard')).toEqual({ kind: 'scorecard' })
  expect(parseCommand('targets')).toEqual({ kind: 'targets' })
  expect(parseCommand('usage')).toEqual({ kind: 'usage' })
  expect(parseCommand('use sonnet gpt-6-sol')).toEqual({ kind: 'use', role: 'sonnet', word: 'gpt-6-sol', scope: 'session' })
  expect(parseCommand('use opus claude-x@y default')).toEqual({ kind: 'use', role: 'opus', word: 'claude-x@y', scope: 'global' })
  expect(parseCommand('use sonnet clear')).toEqual({ kind: 'use', role: 'sonnet', word: 'clear', scope: 'session' })
  expect(parseCommand('use banana x')).toEqual({ kind: 'error', message: 'unknown role banana (opus, sonnet, haiku)' })
  expect(parseCommand('use sonnet')).toEqual({ kind: 'error', message: 'use ROLE TARGET [default]' })
  expect(parseCommand('rm -rf')).toEqual({ kind: 'error', message: 'unknown subcommand rm (status, targets, usage, use, setup, recipes, agents, scorecard, pane)' })
})

test('resolveTarget takes a full id, or an upstream name that names one target', () => {
  expect(resolveTarget(found.targets, 'claude-gpt-6-sol@codex')).toEqual({ id: 'claude-gpt-6-sol@codex' })
  expect(resolveTarget(found.targets, 'gpt-6-sol')).toEqual({ id: 'claude-gpt-6-sol@codex' })
  expect(resolveTarget(found.targets, 'nothing')).toEqual({ refused: 'no target nothing (see /llmgate targets)' })
  const twice = [...found.targets, { id: 'claude-gpt-6-sol@other', label: 'gpt-6-sol (other)', provider: 'other' }]
  expect(resolveTarget(twice, 'gpt-6-sol')).toEqual({ refused: 'gpt-6-sol is offered by more than one provider: claude-gpt-6-sol@codex, claude-gpt-6-sol@other' })
})

test('statusText names the provider and model of the last reply with its markers, or the problem', () => {
  expect(statusText(found)).toBe('work sonnet-9 (fallback)')
  expect(statusText({ ...found, served: { ...found.served!, fallback: false, switched: true } })).toBe('work sonnet-9 (switched)')
  expect(statusText({ ...found, served: undefined })).toBe('no reply yet')
  expect(statusText({ ...found, error: 'the gateway answered 404' })).toBe('the gateway answered 404')
})

test('changes reports a fallback, a new provider, an opened circuit and a blocked limit once, and nothing on the first read', () => {
  expect(changes(undefined, found)).toEqual([])
  expect(changes(found, found)).toEqual([])
  const calm = { ...found, served: { ...found.served!, fallback: false }, providers: found.providers.map(p => ({ ...p, circuit: 'closed' })) }
  expect(changes(calm, found)).toEqual(['llmgate: work answered as a fallback', 'llmgate: codex circuit is open'])
  const moved = { ...calm, served: { ...calm.served!, provider: 'codex' } }
  expect(changes(calm, moved)).toEqual(['llmgate: now served by codex'])
  const blocked = { ...calm, providers: calm.providers.map(p => (p.name === 'work' ? { ...p, limits: { 'anthropic-ratelimit-unified-status': 'rejected' } } : p)) }
  expect(changes(calm, blocked)).toEqual(['llmgate: work limit rejected'])
  expect(changes(calm, { ...found, error: 'down' })).toEqual([])
})

test('describeTargets lists every target with its provider', () => {
  expect(describeTargets(found.targets)).toBe('claude-gpt-6-sol@codex  gpt-6-sol (codex)\nclaude-sonnet-9@work  sonnet-9 (work)')
  expect(describeTargets([])).toBe('no targets offered; add offer to a provider in the llmgate config')
})

test('printableBlock keeps lines, replaces control bytes inside them and cuts at the limit', () => {
  expect(printableBlock('a\nb\u001b[2Jc\r\nd')).toBe('a\nb?[2Jc\nd')
  expect(printableBlock('x'.repeat(50), 10)).toBe('xxxxxxxxxx')
})

const withAgents = parseOverview(
  's1',
  JSON.stringify({
    schema_version: 1,
    roles: [{ role: 'sonnet', via: 'config', providers: ['work'] }],
    targets: [{ id: 'claude-gpt-6-sol@codex', provider: 'codex', upstream: 'gpt-6-sol', label: 'gpt-6-sol (codex)' }],
    providers: [],
    agents: [
      { name: 'reviewer', target: 'claude-gpt-6-sol@codex', description: 'A second opinion', prompt: 'Review it.', tools: ['Read', 'Grep'] },
      { name: 'bad name', target: 'x@y', description: '', prompt: '', tools: [] },
      { name: 'ctl', target: 'claude-gpt-6-sol@codex', description: 'a\u001b[2Jb', prompt: 'p', tools: ['Read', 'rm -rf'] },
    ],
    spawn_rules: [
      { agent: 'Explore', target: 'claude-gpt-6-sol@codex' },
      { agent: 'Plan', target: 'not a target' },
      { agent: '../x', target: 'claude-gpt-6-sol@codex' },
    ],
    spend: { today: [], month: [] },
  }),
)

test('agents and spawn rules are read with plain names, known targets and clean text', () => {
  expect(withAgents.agents.map(a => a.name)).toEqual(['reviewer', 'ctl'])
  expect(withAgents.agents[0]).toEqual({ name: 'reviewer', target: 'claude-gpt-6-sol@codex', description: 'A second opinion', prompt: 'Review it.', tools: ['Read', 'Grep'] })
  expect(withAgents.agents[1].description).toBe('a?[2Jb')
  expect(withAgents.agents[1].tools).toEqual(['Read'])
  expect(withAgents.spawnRules).toEqual([{ agent: 'Explore', target: 'claude-gpt-6-sol@codex' }])
})

test('the who-runs-what table lists roles, agent types and spawn rules with the provider of each target', () => {
  expect(agentTable(withAgents)).toEqual([
    'kind        name      runs on                 provider',
    '----------  --------  ----------------------  --------',
    'role        sonnet    work                    work',
    'agent type  reviewer  claude-gpt-6-sol@codex  codex',
    'agent type  ctl       claude-gpt-6-sol@codex  codex',
    'spawn rule  Explore   claude-gpt-6-sol@codex  codex',
  ])
})
