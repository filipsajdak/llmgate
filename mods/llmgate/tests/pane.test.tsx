import { expect, mock, test } from 'claude-code/testing'

const PANE = {
  plugin: 'llmgate',
  component: 'Pane',
  requestId: 'llmgate',
  viewport: { columns: 120, rows: 40 },
  props: { title: 'llmgate', isFocused: true, bodyColumns: 80, placement: 'dock', scroll: { offset: 0, bodyRows: 30 }, view: {} },
} as const

const overview = (sonnet: string) =>
  JSON.stringify({
    schema_version: 1,
    roles: [
      { role: 'opus', via: 'config', providers: ['work'], upstream: 'opus-9' },
      { role: 'sonnet', via: 'config', providers: [sonnet], upstream: 'sonnet-9' },
      { role: 'haiku', via: 'config', providers: ['work'] },
    ],
    served: { provider: 'work', upstream: 'sonnet-9', source: 'config', fallback: false, switched: true },
    targets: [
      { id: 'claude-gpt-6-sol@codex', provider: 'codex', upstream: 'gpt-6-sol', label: 'gpt-6-sol (codex)' },
      { id: 'claude-sonnet-9@work', provider: 'work', upstream: 'sonnet-9', label: 'sonnet-9 (work)' },
    ],
    providers: [{ name: 'work', attempts: 3, answered: 3, failures: 0, skipped: 0, circuit: 'closed', rate_limits: {} }],
    spend: { today: [{ provider: 'work', model: 'sonnet-9', input: 2000, output: 100, cache_read: 0, cache_creation: 0, requests: 3, cost: 0.01 }], month: [] },
    totals: { today: { input: 2000, output: 100, cache_read: 0, cache_creation: 0, requests: 3 }, month: { input: 2000, output: 100, cache_read: 0, cache_creation: 0, requests: 3 }, budget: { state: 'ok', window: '', used: 0, limit: 0, percent: 0 } },
  })

function gateway(on: Parameters<Parameters<typeof test>[1]>[1], calls: string[], sonnet = 'work') {
  on('http.fetch', ($, e) => {
    calls.push(e.url)
    return { value: { status: 200, ok: true, headers: {}, text: overview(sonnet) } }
  })
}

const env = { ANTHROPIC_BASE_URL: 'http://127.0.0.1:9482/anthropic', LLMGATE_BIN: '/opt/llmgate', LLMGATE_CONFIG: '/etc/llmgate.jsonc' }

test('the command opens the pane, which shows the routes, the last reply and the targets', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  const fetched: string[] = []
  gateway(on, fetched)
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  const answer = await $.command.run({ command: 'llmgate', args: '' })
  expect(answer).toEqual({})

  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  expect(await ui.find({ type: 'Text', text: /last reply: work sonnet-9 \(switched\)/ })).toBeDefined()
  expect(await ui.find({ type: 'Text', text: /sonnet -> work \(sonnet-9\)  via config/ })).toBeDefined()
  expect(await ui.find({ type: 'Text', text: 'gpt-6-sol (codex)' })).toBeDefined()
  expect(await ui.find({ key: 'use-claude-gpt-6-sol@codex' })).toBeDefined()
  expect(fetched).toEqual(['http://127.0.0.1:9482/llmgate/overview?session=abc-123'])
  await ui.unmount()
})

test('use for sonnet runs llmgate by argv for this session and make default runs it without a session', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  gateway(on, [])
  const runs: string[][] = []
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    return { value: { exitCode: 0, stdout: '', stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: '' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'use-claude-gpt-6-sol@codex' })
  await ui.press({ key: 'default-claude-sonnet-9@work' })

  expect(runs).toEqual([
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'use', 'sonnet', 'claude-gpt-6-sol@codex', '--session', 'abc-123'],
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'use', 'sonnet', 'claude-sonnet-9@work'],
  ])
  expect(await ui.find({ type: 'Text', text: /sonnet now uses claude-sonnet-9@work for every session/ })).toBeDefined()
  await ui.unmount()
})

test('a failing llmgate is reported in the pane and the choice of role follows the buttons', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  gateway(on, [])
  on('process.run', () => ({ value: { exitCode: 1, stdout: '', stderr: 'llmgate: the gateway is down\u001b[2J' } }))
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: '' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'role-haiku' })
  expect(await ui.find({ type: 'Button', key: 'use-claude-gpt-6-sol@codex' })).toBeDefined()
  await ui.press({ key: 'clear' })
  expect(await ui.find({ type: 'Text', text: 'llmgate: llmgate: the gateway is down?[2J' })).toBeDefined()
  await ui.unmount()
})

test('the Usage view shows the totals, the spend of today with its price, and the providers', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  gateway(on, [])
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: '' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'view-usage' })

  expect(await ui.find({ type: 'Text', text: /today 2k tokens \(3 req\)/ })).toBeDefined()
  expect(await ui.find({ type: 'Text', text: 'work  sonnet-9  2k in 100 out  3 req  $0.01' })).toBeDefined()
  expect(await ui.find({ type: 'Text', text: 'work  closed  3 tried, 3 answered, 0 failed' })).toBeDefined()
  await ui.press({ key: 'view-now' })
  expect(await ui.find({ key: 'use-claude-gpt-6-sol@codex' })).toBeDefined()
  await ui.unmount()
})

test('with no loopback gateway the mod says so, talks to nothing and runs nothing', async ($, on) => {
  mock.env(on, { ANTHROPIC_BASE_URL: 'https://api.anthropic.com' })
  mock.clock(on)
  let touched = 0
  on('http.fetch', () => {
    touched += 1
    return { value: { status: 200, ok: true, headers: {}, text: '{}' } }
  })
  on('process.run', () => {
    touched += 1
    return { value: { exitCode: 0, stdout: '', stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: false, reason: 'no-surface' } }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: false })
  const answer = await $.command.run({ command: 'llmgate', args: '' })

  expect(String(answer.text)).toContain('no llmgate gateway')
  expect(touched).toBe(0)
})

function started(on: Parameters<Parameters<typeof test>[1]>[1], placed = true) {
  mock.env(on, env)
  mock.clock(on)
  gateway(on, [])
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  const opened = { count: 0 }
  on('ui.open', () => {
    opened.count += 1
    return { value: { isPlaced: placed } }
  })
  on('session.start', (_, e) => ({ cwd: e.cwd }))
  return opened
}

test('subcommands answer as text without opening the pane', async ($, on) => {
  const opened = started(on)
  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })

  const status = await $.command.run({ command: 'llmgate', args: 'status' })
  expect(String(status.text)).toContain('sonnet -> work (sonnet-9)  via config')
  expect(String(status.text)).toContain('llmgate: work sonnet-9 (switched)')
  const targets = await $.command.run({ command: 'llmgate', args: 'targets' })
  expect(String(targets.text)).toContain('claude-gpt-6-sol@codex  gpt-6-sol (codex)')
  const usage = await $.command.run({ command: 'llmgate', args: 'usage' })
  expect(String(usage.text)).toContain('work  sonnet-9  2k in 100 out  3 req  $0.01')
  const wrong = await $.command.run({ command: 'llmgate', args: 'frobnicate' })
  expect(String(wrong.text)).toContain('unknown subcommand frobnicate')
  expect(opened.count).toBe(0)
})

test('use runs llmgate for this session, or for every session with default, and refuses a target that is not offered', async ($, on) => {
  started(on)
  const runs: string[][] = []
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    return { value: { exitCode: 0, stdout: '', stderr: '' } }
  })
  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })

  const one = await $.command.run({ command: 'llmgate', args: 'use sonnet gpt-6-sol' })
  expect(String(one.text)).toBe('sonnet now uses claude-gpt-6-sol@codex for this session')
  const two = await $.command.run({ command: 'llmgate', args: 'use haiku claude-sonnet-9@work default' })
  expect(String(two.text)).toBe('haiku now uses claude-sonnet-9@work for every session')
  const clear = await $.command.run({ command: 'llmgate', args: 'use sonnet clear' })
  expect(String(clear.text)).toBe('sonnet override removed for this session')
  const bad = await $.command.run({ command: 'llmgate', args: 'use sonnet nothing' })
  expect(String(bad.text)).toBe('no target nothing (see /llmgate targets)')

  expect(runs).toEqual([
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'use', 'sonnet', 'claude-gpt-6-sol@codex', '--session', 'abc-123'],
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'use', 'haiku', 'claude-sonnet-9@work'],
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'use', 'sonnet', 'clear', '--session', 'abc-123'],
  ])
})

test('the status entry follows the last reply and a change raises a toast', async ($, on) => {
  started(on)
  const statuses: (string | undefined)[] = []
  const toasts: string[] = []
  on('ui.status', (_, e) => {
    statuses.push(e.text)
    return { value: undefined }
  })
  on('ui.toast', (_, e) => {
    toasts.push(e.text)
    return { value: undefined }
  })
  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'status' })
  expect(statuses.at(-1)).toBe('llmgate: work sonnet-9 (switched)')
  expect(toasts).toEqual([])
})

test('a reply that arrives as a fallback between two reads raises one toast', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  let reads = 0
  on('http.fetch', () => {
    reads += 1
    const document = JSON.parse(overview('work'))
    document.served.fallback = reads > 1
    document.served.switched = false
    return { value: { status: 200, ok: true, headers: {}, text: JSON.stringify(document) } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))
  const toasts: string[] = []
  on('ui.toast', (_, e) => {
    toasts.push(e.text)
    return { value: undefined }
  })
  const statuses: (string | undefined)[] = []
  on('ui.status', (_, e) => {
    statuses.push(e.text)
    return { value: undefined }
  })

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'status' })
  await $.command.run({ command: 'llmgate', args: 'status' })
  await $.command.run({ command: 'llmgate', args: 'status' })

  expect(toasts).toEqual(['llmgate: work answered as a fallback'])
  expect(statuses.at(-1)).toBe('llmgate: work sonnet-9 (fallback)')
})
