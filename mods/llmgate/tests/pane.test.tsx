import { expect, mock, test } from 'claude-code/testing'

const PANE = {
  plugin: 'llmgate',
  component: 'Pane',
  requestId: 'llmgate',
  viewport: { columns: 120, rows: 40 },
  props: { title: 'llmgate', isFocused: true, bodyColumns: 80, placement: 'dock', scroll: { offset: 0, bodyRows: 30 }, view: {} },
} as const

const models = JSON.stringify({
  data: [
    { id: 'opus', display_name: 'opus' },
    { id: 'claude-gpt-6-sol@codex', display_name: 'gpt-6-sol (codex)', description: 'served by the provider codex' },
    { id: 'claude-sonnet-9@work', display_name: 'sonnet-9 (work)', description: 'served by the provider work' },
  ],
})

function gateway(on: Parameters<Parameters<typeof test>[1]>[1], calls: string[], routeFor: (url: string) => string) {
  on('http.fetch', ($, e) => {
    calls.push(e.url)
    if (e.url.includes('/anthropic/v1/models')) return { value: { status: 200, ok: true, headers: {}, text: models } }
    if (e.url.includes('/llmgate/session')) return { value: { status: 200, ok: true, headers: {}, text: '{"provider":"work","upstream":"sonnet-9","source":"config","fallback":false,"switched":true}' } }
    return { value: { status: 200, ok: true, headers: {}, text: routeFor(e.url) } }
  })
}

const env = { ANTHROPIC_BASE_URL: 'http://127.0.0.1:9482/anthropic', LLMGATE_BIN: '/opt/llmgate', LLMGATE_CONFIG: '/etc/llmgate.jsonc' }

test('the command opens the pane, which shows the routes, the last reply and the targets', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  const fetched: string[] = []
  gateway(on, fetched, () => '{"role":"sonnet","via":"config","providers":["work"],"upstream":"sonnet-9"}')
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
  expect(fetched.some(url => url.includes('session=abc-123'))).toBe(true)
  await ui.unmount()
})

test('use for sonnet runs llmgate by argv for this session and make default runs it without a session', async ($, on) => {
  mock.env(on, env)
  mock.clock(on)
  gateway(on, [], () => '{"role":"sonnet","via":"config","providers":["work"]}')
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
  gateway(on, [], () => '{}')
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
