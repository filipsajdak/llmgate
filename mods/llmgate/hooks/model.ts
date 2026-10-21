export const roles = ['opus', 'sonnet', 'haiku'] as const
export type Role = (typeof roles)[number]

export type Target = { id: string; label: string; provider: string }
export type RoleRoute = { role: Role; via: string; providers: string[]; upstream?: string }
export type Served = { provider: string; upstream: string; source: string; fallback: boolean; switched: boolean }
export type Snapshot = { sessionId: string; roles: RoleRoute[]; served?: Served; targets: Target[]; error?: string }
export type Scope = 'session' | 'global'

const longestText = 200
const sessionPattern = /^[A-Za-z0-9-]{1,64}$/
const loopbackHosts = ['127.0.0.1', 'localhost', '[::1]', '::1']
const providerPrefix = 'served by the provider '

export function printable(text: string, limit = longestText): string {
  return [...text.slice(0, limit)].map(c => (c < ' ' || c === '\x7f' ? '?' : c)).join('')
}

export function gatewayRoot(base: string | undefined): string | undefined {
  if (base === undefined) return undefined
  let url: URL
  try {
    url = new URL(base)
  } catch {
    return undefined
  }
  if (url.protocol !== 'http:' || !loopbackHosts.includes(url.hostname)) return undefined
  return `${url.protocol}//${url.host}`
}

function object(value: unknown): Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value) ? (value as Record<string, unknown>) : {}
}

function text(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

export function parseTargets(body: string): Target[] {
  let data: unknown
  try {
    data = object(JSON.parse(body)).data
  } catch {
    return []
  }
  if (!Array.isArray(data)) return []
  return data.flatMap(item => {
    const entry = object(item)
    const id = text(entry.id)
    const description = text(entry.description)
    if (id === '' || !id.includes('@')) return []
    const provider = description.startsWith(providerPrefix) ? description.slice(providerPrefix.length) : id.slice(id.lastIndexOf('@') + 1)
    return [{ id, label: printable(text(entry.display_name) || id), provider: printable(provider) }]
  })
}

export function parseRoute(role: Role, body: string): RoleRoute {
  let entry: Record<string, unknown> = {}
  try {
    entry = object(JSON.parse(body))
  } catch {
    // unreadable reply: the role is shown with no providers
  }
  const providers = Array.isArray(entry.providers) ? entry.providers.filter((p): p is string => typeof p === 'string').map(p => printable(p)) : []
  const upstream = text(entry.upstream)
  return { role, via: printable(text(entry.via) || 'unknown'), providers, ...(upstream === '' ? {} : { upstream: printable(upstream) }) }
}

export function parseServed(body: string): Served | undefined {
  let entry: Record<string, unknown> = {}
  try {
    entry = object(JSON.parse(body))
  } catch {
    return undefined
  }
  const provider = text(entry.provider)
  if (provider === '') return undefined
  return {
    provider: printable(provider),
    upstream: printable(text(entry.upstream)),
    source: printable(text(entry.source)),
    fallback: entry.fallback === true,
    switched: entry.switched === true,
  }
}

export function describeRoute(route: RoleRoute): string {
  const where = route.providers.length === 0 ? 'no provider' : route.providers.join(', ')
  return `${route.role.padEnd(6)} -> ${where}${route.upstream ? ` (${route.upstream})` : ''}  via ${route.via}`
}

export type Change = { role: Role; target: string; scope: Scope } | { role: Role; clear: true; scope: Scope }

export type Invocation = { argv: string[] } | { refused: string }

export function invocation(bin: string | undefined, config: string | undefined, session: string, targets: readonly Target[], change: Change): Invocation {
  if (bin === undefined || bin === '') return { refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' }
  if (!roles.includes(change.role)) return { refused: 'unknown role' }
  if (change.scope === 'session' && !sessionPattern.test(session)) return { refused: 'no session id' }
  const word = 'clear' in change ? 'clear' : change.target
  if (!('clear' in change) && !targets.some(item => item.id === change.target)) return { refused: 'that target is not in the catalogue' }
  const argv = [bin, ...(config ? ['--config', config] : []), 'use', change.role, word, ...(change.scope === 'session' ? ['--session', session] : [])]
  return { argv }
}
