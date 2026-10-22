import { expect, test } from 'claude-code/testing'

import { addInvocation, formFor, kinds, nextStep, validateForm } from '../hooks/setup'

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
  expect(nextStep(bin, config, { ...formFor('claude'), name: 'private' })).toContain('/login')
  expect(nextStep(bin, config, { ...formFor('local'), name: 'thor' })).toBe('nothing more to do')
})

test('every kind has a plain-words description and a form', () => {
  for (const kind of kinds) {
    expect(kind.title.length).toBeGreaterThan(0)
    expect(kind.help.length).toBeGreaterThan(20)
    expect(formFor(kind.id).kind).toBe(kind.id)
  }
})
