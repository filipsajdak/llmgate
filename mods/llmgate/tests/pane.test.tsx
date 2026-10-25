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

const statuses: (string | undefined)[] = []
const toasts: string[] = []

function gateway(on: Parameters<Parameters<typeof test>[1]>[1], calls: string[], sonnet = 'work') {
  statuses.length = 0
  toasts.length = 0
  on('ui.status', (_, e) => {
    statuses.push(e.text)
    return { value: undefined }
  })
  on('ui.toast', (_, e) => {
    toasts.push(e.text)
    return { value: undefined }
  })
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
  expect(await ui.find({ type: 'Text', text: /sonnet  work      sonnet-9  config/ })).toBeDefined()
  expect(await ui.find({ type: 'Text', text: 'gpt-6-sol (codex)' })).toBeDefined()
  expect(await ui.find({ key: 'use-claude-gpt-6-sol@codex' })).toBeDefined()
  expect(fetched).toEqual(Array(2).fill('http://127.0.0.1:9482/llmgate/overview?session=abc-123'))
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
  expect(await ui.find({ type: 'Text', text: 'llmgate: the gateway is down?[2J' })).toBeDefined()
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
  expect(await ui.find({ type: 'Text', text: /^work +sonnet-9 +2k +100 +- +3 +\$0\.01$/ })).toBeDefined()
  expect(await ui.find({ type: 'Text', text: /^work +- +unknown +ready +closed +3 +3 +0 +-$/ })).toBeDefined()
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
  expect(String(status.text)).toContain('sonnet  work      sonnet-9  config')
  expect(String(status.text)).toContain('work sonnet-9 (switched)')
  const targets = await $.command.run({ command: 'llmgate', args: 'targets' })
  expect(String(targets.text)).toContain('claude-gpt-6-sol@codex  gpt-6-sol (codex)')
  const usage = await $.command.run({ command: 'llmgate', args: 'usage' })
  expect(String(usage.text)).toContain('work      sonnet-9  2k  100       -    3  $0.01')
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
  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'status' })
  expect(statuses.at(-1)).toBe('work sonnet-9 (switched)')
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
  statuses.length = 0
  toasts.length = 0
  on('ui.toast', (_, e) => {
    toasts.push(e.text)
    return { value: undefined }
  })
  on('ui.status', (_, e) => {
    statuses.push(e.text)
    return { value: undefined }
  })

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'status' })
  await $.command.run({ command: 'llmgate', args: 'status' })
  await $.command.run({ command: 'llmgate', args: 'status' })

  expect(toasts).toEqual(['llmgate: work answered as a fallback'])
  expect(statuses.at(-1)).toBe('work sonnet-9 (fallback)')
})

async function must(ui: { find: (q: object) => Promise<unknown> }, query: object): Promise<void> {
  if ((await ui.find(query)) === undefined) throw new Error(`not found: ${JSON.stringify(query)}`)
}

const setupOverview = () => {
  const document = JSON.parse(overview('work'))
  document.providers = [
    { name: 'codex', attempts: 0, answered: 0, failures: 0, skipped: 0, circuit: 'closed', rate_limits: {}, labels: ['private'], auth: 'chatgpt-login', ready: false },
    { name: 'work', attempts: 3, answered: 3, failures: 0, skipped: 0, circuit: 'closed', rate_limits: {}, labels: [], auth: 'copilot-login', ready: true },
  ]
  return JSON.stringify(document)
}

test('the Setup view lists providers with their state, and the wizard previews, applies and names the next step', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: setupOverview() } }))
  const runs: { argv: string[]; stdin?: string }[] = []
  on('process.run', (_, e) => {
    runs.push({ argv: [...e.argv], stdin: e.stdin })
    const yes = e.argv.includes('--yes')
    return { value: { exitCode: 0, stdout: yes ? 'written to /c\nok\n' : '+ "codex2": { "preset": "chatgpt" }\nnot written: add --yes to write /c\n', stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'setup' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await must(ui, { type: 'Text', text: /^codex +private +chatgpt-login +needs setup/ })
  await must(ui, { type: 'Text', text: /^work +- +copilot-login +ready/ })

  await ui.press({ key: 'kind-chatgpt' })
  await must(ui, { type: 'Text', text: /ChatGPT Plus or Pro plan/ })
  await ui.press({ key: 'preview' })
  await must(ui, { type: 'Text', text: 'give the provider a name (letters, digits, - and _)' })
  expect(runs).toEqual([])

  await ui.input({ key: 'field-name', text: 'codex2' })
  await ui.press({ key: 'preview' })
  await must(ui, { type: 'Text', text: '+ "codex2": { "preset": "chatgpt" }' })
  await ui.press({ key: 'apply' })

  const preview = ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'provider', 'add', 'codex2', '--preset', 'chatgpt']
  expect(runs.map(r => r.argv)).toEqual([preview, preview, [...preview, '--yes']])
  await must(ui, { type: 'Text', text: '! /opt/llmgate --config /etc/llmgate.jsonc login --provider codex2' })
  await ui.unmount()
})

test('a Copilot provider is signed in from the pane: the link and code appear, the view follows the sign-in and then checks the provider', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: setupOverview() } }))
  const runs: string[][] = []
  let polls = 0
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    if (e.argv.includes('--detach')) {
      return { value: { exitCode: 0, stdout: '{"event":"link","url":"https://github.com/login/device","code":"ABCD-1234"}\n', stderr: '' } }
    }
    if (e.argv.includes('--progress')) {
      polls += 1
      return { value: { exitCode: 0, stdout: polls < 2 ? '{"event":"link","url":"https://github.com/login/device","code":"ABCD-1234"}\n' : '{"event":"done","detail":"the token is stored in /t.json"}\n', stderr: '' } }
    }
    if (e.argv.includes('check')) {
      return { value: { exitCode: 0, stdout: JSON.stringify([{ provider: 'seat', step: 'ping', result: 'pass', milliseconds: 900, detail: '' }]), stderr: '' } }
    }
    return { value: { exitCode: 0, stdout: 'written to /c\nok\n', stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'setup' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'kind-copilot' })
  await ui.input({ key: 'field-name', text: 'seat' })
  await ui.press({ key: 'apply' })
  await ui.press({ key: 'sign-in' })

  await must(ui, { type: 'Text', text: 'Open https://github.com/login/device and enter the code ABCD-1234' })
  await $.command.run({ command: 'llmgate', args: 'status' })
  await must(ui, { type: 'Text', text: 'Waiting for you to authorise (this view follows by itself)...' })
  await $.command.run({ command: 'llmgate', args: 'status' })
  await must(ui, { type: 'Text', text: 'Signed in: the token is stored in /t.json' })
  await must(ui, { type: 'Text', text: /seat +ping +pass/ })

  const base = ['/opt/llmgate', '--config', '/etc/llmgate.jsonc']
  expect(runs).toContainEqual([...base, 'login', '--provider', 'seat', '--json', '--detach'])
  expect(runs).toContainEqual([...base, 'check', '--json', '--quick', '--provider', 'seat'])
  await ui.unmount()
})

test('the model list of a provider is ticked in the pane and applied as one models offer', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: setupOverview() } }))
  const runs: string[][] = []
  const listing = JSON.stringify([
    { id: 'claude-sonnet-5.5', context: 328000, prompt: 200000, output: 128000, policy: 'disabled', protocol: 'anthropic', recommended: false, offered: false },
    { id: 'gpt-6-luna', context: 400000, prompt: 272000, output: 128000, policy: 'enabled', protocol: 'openai_responses', recommended: true, offered: false },
    { id: 'gpt-5.4-nano', context: 400000, prompt: 272000, output: 128000, policy: 'enabled', protocol: 'openai_responses', recommended: true, offered: true },
  ])
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    return { value: { exitCode: 0, stdout: e.argv.includes('list') ? listing : 'written to /c\nok\n', stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'setup' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'models-work' })
  await must(ui, { type: 'Button', key: 'tick-gpt-6-luna', label: '[ ] gpt-6-luna  openai_responses  400k tokens  recommended' })
  await must(ui, { type: 'Button', key: 'tick-gpt-5.4-nano', label: '[x] gpt-5.4-nano  openai_responses  400k tokens  recommended' })
  await must(ui, { type: 'Button', key: 'tick-claude-sonnet-5.5', label: '[ ] claude-sonnet-5.5  anthropic  328k tokens  disabled by the account' })

  await ui.press({ key: 'tick-gpt-6-luna' })
  await ui.press({ key: 'tick-claude-sonnet-5.5' })
  await must(ui, { type: 'Button', key: 'tick-gpt-6-luna', label: '[x] gpt-6-luna  openai_responses  400k tokens  recommended' })
  await must(ui, { type: 'Button', key: 'tick-claude-sonnet-5.5', label: '[ ] claude-sonnet-5.5  anthropic  328k tokens  disabled by the account' })
  await ui.press({ key: 'models-apply' })

  expect(runs).toContainEqual(['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'models', 'list', 'work', '--json'])
  expect(runs).toContainEqual(['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'models', 'offer', 'work', 'gpt-6-luna', 'gpt-5.4-nano', '--yes'])
  await ui.unmount()
})

test('an API key provider names the secret command and never asks for the key in the pane', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: setupOverview() } }))
  on('process.run', () => ({ value: { exitCode: 0, stdout: 'ok\n', stderr: '' } }))
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'setup' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'kind-apikey' })
  await ui.input({ key: 'field-name', text: 'partner' })
  await ui.input({ key: 'field-baseUrl', text: 'https://gw.example.net' })
  await ui.press({ key: 'apply' })

  await must(ui, { type: 'Text', text: '! /opt/llmgate secret set partner' })
  await must(ui, { type: 'Text', text: /hidden/ })
  await ui.unmount()
})

test('adding a Claude login provider also sets the credential mode to login and says to restart', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: setupOverview() } }))
  const runs: string[][] = []
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    return { value: { exitCode: 0, stdout: 'ok\n', stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'setup' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'kind-claude' })
  await ui.input({ key: 'field-name', text: 'private' })
  runs.length = 0
  await ui.press({ key: 'apply' })

  expect(runs).toEqual([
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'provider', 'add', 'private', '--preset', 'anthropic', '--yes'],
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'config', 'set', 'claude.credential', 'login', '--yes'],
  ])
  await must(ui, { type: 'Text', text: /restart Claude Code/ })
  await ui.unmount()
})

test('the Recipes view lists cards with pros and cons and previews then applies one by argv', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: setupOverview() } }))
  const runs: string[][] = []
  const list = JSON.stringify([{ id: 'quality-first', title: 'Quality first, with fallback', summary: 'Claude answers every role.', pros: ['one vendor'], cons: ['spends Claude quota'], needs: ['claude'], tokens: '1x' }])
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    if (e.argv.includes('list')) return { value: { exitCode: 0, stdout: list, stderr: '' } }
    return { value: { exitCode: 0, stdout: e.argv.includes('--yes') ? 'written to /c\nok\n' : '+ "routes"\nnot written: add --yes to write /c\n', stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'recipes' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await must(ui, { type: 'Text', text: 'Quality first, with fallback' })
  await must(ui, { type: 'Text', text: '+ one vendor' })
  await must(ui, { type: 'Text', text: '- spends Claude quota' })
  await ui.press({ key: 'recipe-quality-first' })
  await must(ui, { type: 'Text', text: 'not written: add --yes to write /c' })
  await ui.press({ key: 'recipe-apply' })
  await must(ui, { type: 'Text', text: 'ok' })

  expect(runs).toEqual([
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'recipe', 'list', '--json'],
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'recipe', 'apply', 'quality-first'],
    ['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'recipe', 'apply', 'quality-first', '--yes'],
  ])
  await ui.unmount()
})

const agentOverview = () => {
  const document = JSON.parse(overview('work'))
  document.agents = [{ name: 'reviewer', target: 'claude-gpt-6-sol@codex', description: 'A second opinion', prompt: 'Review it.', tools: ['Read', 'Grep'] }]
  document.spawn_rules = [{ agent: 'Explore', target: 'claude-gpt-6-sol@codex' }]
  return JSON.stringify(document)
}

test('the mod registers the configured agent types once and a spawn rule sets the model of the subagent', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: agentOverview() } }))
  const registered: { name: string; model?: string; description: string; prompt: string; tools?: readonly string[] }[] = []
  on('agent.register', (_, e) => {
    registered.push({ name: e.name, model: e.model, description: e.description, prompt: e.prompt, tools: e.tools })
    return { value: { agent: `llmgate:${e.name}` } }
  })
  const models: (string | undefined)[] = []
  on('agent.spawn', (_, e) => {
    models.push(e.model)
    return { model: e.model ?? 'main', agentId: 'a1' }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'status' })
  await $.command.run({ command: 'llmgate', args: 'status' })
  expect(registered.filter(item => item.name !== 'at')).toEqual([{ name: 'reviewer', model: 'claude-gpt-6-sol@codex', description: 'A second opinion', prompt: 'Review it.', tools: ['Read', 'Grep'] }])

  await $.agent.spawn({ prompt: 'look around', subagentType: 'Explore' })
  await $.agent.spawn({ prompt: 'plan it', subagentType: 'Plan' })
  expect(models).toEqual(['claude-gpt-6-sol@codex', undefined])
})

test('the Agents view and /llmgate agents show who runs what', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: agentOverview() } }))
  on('agent.register', (_, e) => ({ value: { agent: `llmgate:${e.name}` } }))
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  const text = await $.command.run({ command: 'llmgate', args: 'agents' })
  expect(String(text.text)).toContain('agent type  reviewer')
  expect(String(text.text)).toContain('spawn rule  Explore')
  await $.command.run({ command: 'llmgate', args: 'pane' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'view-agents' })
  await must(ui, { type: 'Text', text: /^agent type +reviewer +claude-gpt-6-sol@codex +codex$/ })
  await ui.unmount()
})

const scoreOverview = () => {
  const document = JSON.parse(setupOverview())
  const row = (name: string, extra: object = {}) => ({ name, input: 1200, output: 300, cache_read: 3600, cache_creation: 0, requests: 4, average_ms: 250, max_ms: 900, failures: 1, fallbacks: 2, share_percent: 80, ...extra })
  document.scorecard = {
    today: { by_provider: [row('work')], by_class: [row('subagent')], by_agent: [row('custom')], by_label: [row('work')] },
    month: { by_provider: [row('work', { requests: 40 })], by_class: [], by_agent: [], by_label: [] },
  }
  return JSON.stringify(document)
}

test('the Scorecard tab shows the meters and four tables, and the month button switches the window', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: scoreOverview() } }))
  const runs: string[][] = []
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    return { value: { exitCode: 0, stdout: JSON.stringify({ plan: 'business', sku: 'x', reset_date: '2026-11-01', quotas: [{ name: 'premium_interactions', unlimited: false, has_quota: true, entitlement: 300, remaining: 120, percent_remaining: 40 }] }), stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  const text = await $.command.run({ command: 'llmgate', args: 'scorecard' })
  expect(String(text.text)).toContain('premium_interactions 120 of 300 left (40%)')
  expect(String(text.text)).toContain('By provider')
  await $.command.run({ command: 'llmgate', args: 'pane' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'view-scorecard' })
  await must(ui, { type: 'Text', text: 'By request class' })
  await must(ui, { type: 'Text', text: /^subagent +4 +80% +1k +300 +75%/ })
  await ui.press({ key: 'window-month' })
  await must(ui, { type: 'Text', text: /^work +40 +80%/ })
  expect(runs[0]).toEqual(['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'quota', '--json', '--provider', 'work'])
  await ui.unmount()
})

test('the quick check asks first, then runs llmgate check and shows the table', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: scoreOverview() } }))
  const runs: string[][] = []
  on('process.run', (_, e) => {
    runs.push([...e.argv])
    return { value: { exitCode: 1, stdout: JSON.stringify([{ provider: 'work', step: 'ping', result: 'pass', detail: '', milliseconds: 800 }, { provider: 'work', step: 'tool', result: 'fail', detail: 'no tool call came back', milliseconds: 1500 }]), stderr: '' } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'pane' })
  const ui = await $.ui.mount({ ...PANE, surface: 'terminal' })
  await ui.press({ key: 'view-scorecard' })
  await ui.press({ key: 'check-quick' })
  await must(ui, { type: 'Text', text: /sends one small request to each provider \(codex, work\)/ })
  expect(runs.some(r => r.includes('check'))).toBe(false)
  await ui.press({ key: 'check-run' })
  await must(ui, { type: 'Text', text: /^work +tool +FAIL +1\.5 s +no tool call came back$/ })
  expect(runs.filter(r => r.includes('check'))).toEqual([['/opt/llmgate', '--config', '/etc/llmgate.jsonc', 'check', '--json', '--quick']])
  await ui.unmount()
})

test('an @target: prefix hands the task to the at agent, whose one spawn runs on that target and then falls back to the session model', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: overview('work') } }))
  const registered: { name: string; model?: string }[] = []
  on('agent.register', (_, e) => {
    registered.push({ name: e.name, model: e.model })
    return { value: { agent: `llmgate:${e.name}` } }
  })
  const models: (string | undefined)[] = []
  on('agent.spawn', (_, e) => {
    models.push(e.model)
    return { model: e.model ?? 'main', agentId: 'a1' }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))
  on('prompt.submit', (_, e) => ({ text: e.text }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'status' })
  const handed = await $.prompt.submit({ text: '@work: check the parser' })
  const refused = await $.prompt.submit({ text: '@nowhere: x' })
  await $.agent.spawn({ prompt: 'check the parser', subagentType: 'llmgate:at' })
  await $.agent.spawn({ prompt: 'again', subagentType: 'llmgate:at' })

  expect(registered).toContainEqual({ name: 'at', model: undefined })
  expect(handed.text).toContain('llmgate:at')
  expect(handed.text.endsWith('check the parser')).toBe(true)
  expect(refused.text).toBe('@nowhere: x')
  expect(models).toEqual(['claude-sonnet-9@work', undefined])
})

test('a prompt that starts with council: is rewritten to dispatch the registered council members, and other prompts pass untouched', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  const document = JSON.parse(agentOverview())
  document.agents = [
    { name: 'council-chatgpt', target: 'claude-gpt-6-sol@codex', description: 'd', prompt: 'p', tools: ['Read'], max_turns: 8 },
    { name: 'reviewer', target: 'claude-gpt-6-sol@codex', description: 'd', prompt: 'p', tools: [], max_turns: 0 },
  ]
  on('http.fetch', () => ({ value: { status: 200, ok: true, headers: {}, text: JSON.stringify(document) } }))
  const registered: { name: string; maxTurns?: number }[] = []
  on('agent.register', (_, e) => {
    registered.push({ name: e.name, maxTurns: e.maxTurns })
    return { value: { agent: `llmgate:${e.name}` } }
  })
  on('command.register', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'abc-123' }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))
  on('prompt.submit', (_, e) => ({ text: e.text }))

  await $.session.start({ cwd: '/', surface: 'terminal', isInteractive: true })
  await $.command.run({ command: 'llmgate', args: 'status' })
  const rewritten = await $.prompt.submit({ text: 'council: should we split the service?' })
  const plain = await $.prompt.submit({ text: 'just a normal question' })

  expect(registered.filter(item => item.name !== 'at')).toEqual([{ name: 'council-chatgpt', maxTurns: 8 }, { name: 'reviewer', maxTurns: undefined }])
  expect(rewritten.text).toContain('llmgate:council-chatgpt')
  expect(rewritten.text).not.toContain('reviewer')
  expect(rewritten.text.endsWith('should we split the service?')).toBe(true)
  expect(plain.text).toBe('just a normal question')
})
