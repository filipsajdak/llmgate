import { expect, test } from 'claude-code/testing'

import { addInvocation, candidateLabel, candidatesInvocation, followUp, formFor, kinds, loginInvocation, nextStep, offerInvocation, parseCandidates, parseLoginEvent, signInLines, validateForm } from '../hooks/setup'

const bin = '/opt/llmgate'
const config = '/etc/all.jsonc'

test('each kind turns its form into one llmgate provider add argv, never a shell string', () => {
  expect(addInvocation(bin, config, { ...formFor('chatgpt'), name: 'codex' }, false)).toEqual({
    argv: [bin, '--config', config, 'provider', 'add', 'codex', '--preset', 'chatgpt'],
  })
  expect(addInvocation(bin, config, { ...formFor('copilot'), name: 'work', gheHost: 'corp.ghe.example', labels: 'work,acme' }, true)).toEqual({
    argv: [bin, '--config', config, 'provider', 'add', 'work', '--preset', 'copilot', '--ghe-host', 'corp.ghe.example', '--labels', 'work,acme', '--yes'],
  })
  expect(addInvocation(bin, config, { ...formFor('claude'), name: 'private' }, false)).toEqual({ argv: [bin, '--config', config, 'provider', 'add', 'private', '--preset', 'anthropic'] })
  expect(addInvocation(bin, config, { ...formFor('apikey'), name: 'partner', baseUrl: 'https://gw.example.net', header: 'api-key', offer: 'gpt-sol,gpt-luna' }, false)).toEqual({
    argv: [bin, '--config', config, 'provider', 'add', 'partner', '--base-url', 'https://gw.example.net', '--auth', 'header:api-key', '--secret', 'file:~/.config/llmgate/secrets/partner.key', '--offer', 'gpt-sol,gpt-luna'],
  })
  expect(addInvocation(bin, config, { ...formFor('local'), name: 'thor', baseUrl: 'http://127.0.0.1:30800' }, false)).toEqual({
    argv: [bin, '--config', config, 'provider', 'add', 'thor', '--base-url', 'http://127.0.0.1:30800', '--auth', 'none'],
  })
  expect(addInvocation(bin, config, { ...formFor('gateway'), name: 'mic', baseUrl: 'http://127.0.0.1:9080', offer: 'opus4.6-partner' }, false)).toEqual({
    argv: [bin, '--config', config, 'provider', 'add', 'mic', '--base-url', 'http://127.0.0.1:9080', '--auth', 'none', '--offer', 'opus4.6-partner'],
  })
})

test('a form is refused with a plain reason before anything runs', () => {
  expect(validateForm({ ...formFor('chatgpt'), name: '' })).toBe('give the provider a name (letters, digits, - and _)')
  expect(validateForm({ ...formFor('chatgpt'), name: 'a b' })).toBe('give the provider a name (letters, digits, - and _)')
  expect(validateForm({ ...formFor('apikey'), name: 'n', baseUrl: 'ftp://x' })).toBe('the address must start with http:// or https://')
  expect(validateForm({ ...formFor('apikey'), name: 'n', baseUrl: 'https://x', header: 'bad name' })).toBe('the header name is letters, digits and -')
  expect(validateForm({ ...formFor('copilot'), name: 'w', gheHost: 'https://corp' })).toBe('the GitHub host is a bare host name such as corp.ghe.example')
  expect(validateForm({ ...formFor('chatgpt'), name: 'x', labels: 'bad label' })).toBe('labels are letters, digits, - and _ separated by commas')
  expect(validateForm({ ...formFor('apikey'), name: 'n', baseUrl: 'https://x', offer: 'a b' })).toBe('model names have no spaces; separate them with commas')
  expect(validateForm({ ...formFor('chatgpt'), name: 'ok' })).toBeUndefined()
  expect(addInvocation(undefined, undefined, { ...formFor('chatgpt'), name: 'ok' }, false)).toEqual({ refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' })
})

test('the next step names the exact command to type, with the hidden prompt for a secret', () => {
  expect(nextStep(bin, config, { ...formFor('chatgpt'), name: 'codex' })).toBe(`! ${bin} --config ${config} login --provider codex`)
  expect(nextStep(bin, config, { ...formFor('apikey'), name: 'partner' })).toBe(`! ${bin} secret set partner`)
  expect(nextStep(bin, config, { ...formFor('claude'), name: 'private' })).toContain('restart Claude Code')
  expect(followUp(bin, config, { ...formFor('claude'), name: 'private' })).toEqual([bin, '--config', config, 'config', 'set', 'claude.credential', 'login', '--yes'])
  expect(followUp(bin, config, { ...formFor('chatgpt'), name: 'x' })).toBeUndefined()
  expect(nextStep(bin, config, { ...formFor('local'), name: 'thor' })).toBe('nothing more to do')
})

test('every kind has a plain-words description and a form', () => {
  for (const kind of kinds) {
    expect(kind.title.length).toBeGreaterThan(0)
    expect(kind.help.length).toBeGreaterThan(20)
    expect(formFor(kind.id).kind).toBe(kind.id)
  }
})

test('sign-in runs login by argv, first detached for the link and then for its progress', () => {
  expect(loginInvocation(bin, config, 'work', 'detach')).toEqual({ argv: [bin, '--config', config, 'login', '--provider', 'work', '--json', '--detach'] })
  expect(loginInvocation(bin, config, 'work', 'progress')).toEqual({ argv: [bin, '--config', config, 'login', '--provider', 'work', '--json', '--progress'] })
  expect(loginInvocation(undefined, config, 'work', 'detach')).toEqual({ refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' })
})

test('a login event is read from the last JSON line, and anything else is not an event', () => {
  expect(parseLoginEvent('{"event":"link","url":"https://github.com/login/device","code":"ABCD-1234"}\n')).toEqual({
    event: 'link',
    url: 'https://github.com/login/device',
    code: 'ABCD-1234',
  })
  expect(parseLoginEvent('noise\n{"event":"link","url":"u"}\n{"event":"done","detail":"stored"}\n')).toEqual({ event: 'done', detail: 'stored' })
  expect(parseLoginEvent('not json')).toBeUndefined()
  expect(parseLoginEvent('{"event":"rm -rf"}')).toBeUndefined()
})

test('the pane lines for a sign-in name the link and the code, the result, or the failure, with control characters removed', () => {
  expect(signInLines({ event: 'link', url: 'https://github.com/login/device', code: 'ABCD-1234' })).toEqual([
    'Open https://github.com/login/device and enter the code ABCD-1234',
    'Waiting for you to authorise (this view follows by itself)...',
  ])
  expect(signInLines({ event: 'link', url: 'https://auth.example/x' })).toEqual(['Open https://auth.example/x in your browser', 'Waiting for the browser to come back (this view follows by itself)...'])
  expect(signInLines({ event: 'done', detail: 'the token is stored in /t.json' })).toEqual(['Signed in: the token is stored in /t.json'])
  expect(signInLines({ event: 'failed', detail: 'bad\x1b[31m' })).toEqual(['Sign-in failed: bad?[31m'])
})

const listing = JSON.stringify([
  { id: 'claude-sonnet-5.5', context: 328000, prompt: 200000, output: 128000, policy: 'disabled', protocol: 'anthropic', recommended: false, offered: false },
  { id: 'gpt-6-luna', context: 400000, prompt: 272000, output: 128000, policy: 'enabled', protocol: 'openai_responses', recommended: true, offered: true },
  { id: 'old-chat', context: 0, prompt: 0, output: 0, policy: 'unknown', protocol: 'unsupported', recommended: false, offered: false },
])

test('the model list is read from llmgate models list, and a reply that is not a list is empty', () => {
  expect(candidatesInvocation(bin, config, 'work')).toEqual({ argv: [bin, '--config', config, 'models', 'list', 'work', '--json'] })
  expect(parseCandidates(listing).map(c => c.id)).toEqual(['claude-sonnet-5.5', 'gpt-6-luna', 'old-chat'])
  expect(parseCandidates('nope')).toEqual([])
  expect(parseCandidates('{"id":"x"}')).toEqual([])
})

test('each row says whether it is ticked, its protocol, its window and why it cannot be', () => {
  const rows = parseCandidates(listing)
  expect(candidateLabel(rows[1], true)).toBe('[x] gpt-6-luna  openai_responses  400k tokens  recommended')
  expect(candidateLabel(rows[0], false)).toBe('[ ] claude-sonnet-5.5  anthropic  328k tokens  disabled by the account')
  expect(candidateLabel(rows[2], false)).toBe('[ ] old-chat  not translated yet  window unknown')
})

test('offering the ticked models is one llmgate models offer argv, and nothing ticked is refused', () => {
  expect(offerInvocation(bin, config, 'work', ['gpt-6-luna', 'claude-sonnet-5.5'])).toEqual({
    argv: [bin, '--config', config, 'models', 'offer', 'work', 'gpt-6-luna', 'claude-sonnet-5.5', '--yes'],
  })
  expect(offerInvocation(bin, config, 'work', [])).toEqual({ refused: 'tick at least one model' })
})

test('a model id that could be read as an option or holds odd characters is dropped from the list and refused for an offer', () => {
  const odd = JSON.stringify([
    { id: '--config=/tmp/x', context: 0, policy: 'enabled', protocol: 'anthropic', recommended: true, offered: false },
    { id: 'has space', context: 0, policy: 'enabled', protocol: 'anthropic', recommended: true, offered: false },
    { id: 'fine-1.5', context: 0, policy: 'enabled', protocol: 'anthropic', recommended: true, offered: false },
  ])
  expect(parseCandidates(odd).map(c => c.id)).toEqual(['fine-1.5'])
  expect(offerInvocation(bin, config, 'work', ['--config=/tmp/x'])).toEqual({ refused: 'a model name may hold letters, digits and . _ : / @ + - only, and must not start with -' })
})
