import { printable } from './model'

export type KindId = 'claude' | 'chatgpt' | 'copilot' | 'apikey' | 'local' | 'gateway'

export type Kind = { id: KindId; title: string; help: string }

export const kinds: readonly Kind[] = [
  { id: 'claude', title: 'Claude login', help: 'Uses the Claude account you sign in to with /login in this profile. llmgate forwards your own login and never keeps it.' },
  { id: 'chatgpt', title: 'ChatGPT plan', help: 'Uses your ChatGPT Plus or Pro plan (Codex models) after a one-time sign-in in your browser.' },
  { id: 'copilot', title: 'GitHub Copilot', help: 'Uses a Copilot seat after a one-time device sign-in; give the company GitHub host if the seat is on one. A seat that has only GPT models, no Claude ones, needs the provider protocol set to openai_responses (config set providers.NAME.protocol openai_responses).' },
  { id: 'apikey', title: 'API key', help: 'A service that wants a key in a header, such as a company gateway. The key is stored in a file only you can read.' },
  { id: 'local', title: 'Local model', help: 'A model on your own machine or network that needs no sign-in.' },
  { id: 'gateway', title: 'Another gateway', help: 'A gateway that already speaks the Claude API on this machine, for example the local gateway; its model names become targets. Give the address with the path prefix it serves the API under (local-gateway: http://127.0.0.1:9080/anthropic).' },
]

export type Form = { kind: KindId; name: string; baseUrl: string; header: string; scheme: string; gheHost: string; labels: string; offer: string }

const emptyForm: Omit<Form, 'kind'> = { name: '', baseUrl: '', header: 'api-key', scheme: '', gheHost: '', labels: '', offer: '' }

export function formFor(kind: KindId): Form {
  return { kind, ...emptyForm }
}

const namePattern = /^[A-Za-z0-9_-]{1,48}$/
const labelPattern = /^[A-Za-z0-9_-]{1,24}(,[A-Za-z0-9_-]{1,24})*$/
const headerPattern = /^[A-Za-z0-9-]{1,64}$/
const hostPattern = /^[A-Za-z0-9.-]{1,253}$/
const modelPattern = /^[^\s,]{1,96}(,[^\s,]{1,96})*$/

export function validateForm(form: Form): string | undefined {
  if (!namePattern.test(form.name)) return 'give the provider a name (letters, digits, - and _)'
  const needsAddress = form.kind === 'apikey' || form.kind === 'local' || form.kind === 'gateway'
  if (needsAddress && !/^https?:\/\/[^\s]+$/.test(form.baseUrl)) return 'the address must start with http:// or https://'
  if (form.kind === 'apikey' && !headerPattern.test(form.header)) return 'the header name is letters, digits and -'
  if (form.kind === 'copilot' && form.gheHost !== '' && !hostPattern.test(form.gheHost)) return 'the GitHub host is a bare host name such as corp.ghe.example'
  if (form.labels !== '' && !labelPattern.test(form.labels)) return 'labels are letters, digits, - and _ separated by commas'
  if (form.offer !== '' && !modelPattern.test(form.offer)) return 'model names have no spaces; separate them with commas'
  return undefined
}

export type Plan = { argv: string[] } | { refused: string }

function words(form: Form): string[] {
  const out: string[] = []
  if (form.kind === 'claude') out.push('--preset', 'anthropic')
  if (form.kind === 'chatgpt') out.push('--preset', 'chatgpt')
  if (form.kind === 'copilot') {
    out.push('--preset', 'copilot')
    if (form.gheHost !== '') out.push('--ghe-host', form.gheHost)
  }
  if (form.kind === 'apikey' || form.kind === 'local' || form.kind === 'gateway') out.push('--base-url', form.baseUrl)
  if (form.kind === 'apikey') {
    out.push('--auth', form.scheme === '' ? `header:${form.header}` : `header:${form.header}:${form.scheme}`)
    out.push('--secret', `file:~/.config/llmgate/secrets/${form.name}.key`)
  }
  if (form.kind === 'local' || form.kind === 'gateway') out.push('--auth', 'none')
  if (form.offer !== '') out.push('--offer', form.offer)
  if (form.labels !== '') out.push('--labels', form.labels)
  return out
}

export function addInvocation(bin: string | undefined, config: string | undefined, form: Form, write: boolean): Plan {
  if (bin === undefined || bin === '') return { refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' }
  const problem = validateForm(form)
  if (problem !== undefined) return { refused: problem }
  return { argv: [bin, ...(config ? ['--config', config] : []), 'provider', 'add', form.name, ...words(form), ...(write ? ['--yes'] : [])] }
}

export function followUp(bin: string, config: string | undefined, form: Form): string[] | undefined {
  if (form.kind !== 'claude') return undefined
  return [bin, ...(config ? ['--config', config] : []), 'config', 'set', 'claude.credential', 'login', '--yes']
}

export function nextStep(bin: string, config: string | undefined, form: Form): string {
  const base = config ? `${bin} --config ${config}` : bin
  if (form.kind === 'chatgpt' || form.kind === 'copilot') return `! ${base} login --provider ${form.name}`
  if (form.kind === 'apikey') return `! ${bin} secret set ${form.name}`
  if (form.kind === 'claude') return 'restart Claude Code, then type /login once and pick the Claude account (the credential mode is now login)'
  return 'nothing more to do'
}

export function describeForm(form: Form): string {
  return printable(`${form.kind} ${form.name}`)
}

export type LoginEvent = { event: 'link' | 'done' | 'failed' | 'idle'; url?: string; code?: string; detail?: string }

const loginEvents: readonly string[] = ['link', 'done', 'failed', 'idle']

export function loginInvocation(bin: string | undefined, config: string | undefined, name: string, mode: 'detach' | 'progress'): Plan {
  if (bin === undefined || bin === '') return { refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' }
  return { argv: [bin, ...(config ? ['--config', config] : []), 'login', '--provider', name, '--json', `--${mode}`] }
}

export function parseLoginEvent(text: string): LoginEvent | undefined {
  for (const line of text.split('\n').reverse()) {
    if (!line.startsWith('{')) continue
    try {
      const found = JSON.parse(line) as Record<string, unknown>
      if (typeof found.event !== 'string' || !loginEvents.includes(found.event)) return undefined
      const text = (key: string) => (typeof found[key] === 'string' ? { [key]: found[key] as string } : {})
      return { event: found.event as LoginEvent['event'], ...text('url'), ...text('code'), ...text('detail') }
    } catch {
      return undefined
    }
  }
  return undefined
}

export function signInLines(item: LoginEvent): string[] {
  if (item.event === 'link' && item.code !== undefined) {
    return [`Open ${printable(item.url ?? '')} and enter the code ${printable(item.code)}`, 'Waiting for you to authorise (this view follows by itself)...']
  }
  if (item.event === 'link') {
    return [`Open ${printable(item.url ?? '')} in your browser`, 'Waiting for the browser to come back (this view follows by itself)...']
  }
  if (item.event === 'done') return [`Signed in: ${printable(item.detail ?? '')}`]
  if (item.event === 'failed') return [`Sign-in failed: ${printable(item.detail ?? 'no reason was given')}`]
  return ['No sign-in is under way']
}
