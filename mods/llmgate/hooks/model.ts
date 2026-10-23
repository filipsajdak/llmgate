export const roles = ['opus', 'sonnet', 'haiku'] as const
export type Role = (typeof roles)[number]

export type Target = { id: string; label: string; provider: string }
export type RoleRoute = { role: Role; via: string; providers: string[]; upstream?: string }
export type Served = { provider: string; upstream: string; source: string; fallback: boolean; switched: boolean }
export type ProviderHealth = { name: string; circuit: string; attempts: number; answered: number; failures: number; limits: Record<string, string>; labels: string[]; auth: string; ready: boolean }
export type SpendRow = { provider: string; model: string; input: number; output: number; cacheRead: number; requests: number; cost?: number }
export type Totals = { todayTokens: number; todayRequests: number; monthTokens: number; budgetState: string; budgetWindow: string; budgetPercent: number; budgetLimit: number }
export type ScoreRow = { name: string; requests: number; input: number; output: number; cacheRead: number; averageMs: number; maxMs: number; failures: number; fallbacks: number; share: number; cost?: number }
export type Scores = { byProvider: ScoreRow[]; byClass: ScoreRow[]; byAgent: ScoreRow[]; byLabel: ScoreRow[] }
export type Scorecard = { today: Scores; month: Scores }
export type Agent = { name: string; target: string; description: string; prompt: string; tools: string[] }
export type SpawnRule = { agent: string; target: string }
export type Snapshot = {
  sessionId: string
  roles: RoleRoute[]
  served?: Served
  targets: Target[]
  providers: ProviderHealth[]
  spendToday: SpendRow[]
  spendMonth: SpendRow[]
  agents: Agent[]
  spawnRules: SpawnRule[]
  scorecard: Scorecard
  totals?: Totals
  error?: string
}
export type Scope = 'session' | 'global'

const longestText = 200
const sessionPattern = /^[A-Za-z0-9-]{1,64}$/
const loopbackHosts = ['127.0.0.1', 'localhost', '[::1]', '::1']

export function printable(text: string, limit = longestText): string {
  return [...text.slice(0, limit)].map(c => (c < ' ' || c === '\x7f' ? '?' : c)).join('')
}

export function printableBlock(text: string, limit = 1200): string {
  return text
    .slice(0, limit)
    .split('\n')
    .map(row => printable(row.replace(/\r$/, '')))
    .join('\n')
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

function count(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : 0
}

function list(value: unknown): unknown[] {
  return Array.isArray(value) ? value : []
}

const overviewVersion = 1

const noScores: Scores = { byProvider: [], byClass: [], byAgent: [], byLabel: [] }
export const noScorecard: Scorecard = { today: noScores, month: noScores }

function parseScoreRow(value: unknown): ScoreRow[] {
  const entry = object(value)
  const name = printable(text(entry.name), 48)
  if (name === '') return []
  const cost = entry.cost
  return [
    {
      name,
      requests: count(entry.requests),
      input: count(entry.input),
      output: count(entry.output),
      cacheRead: count(entry.cache_read),
      averageMs: count(entry.average_ms),
      maxMs: count(entry.max_ms),
      failures: count(entry.failures),
      fallbacks: count(entry.fallbacks),
      share: count(entry.share_percent),
      ...(typeof cost === 'number' && Number.isFinite(cost) ? { cost } : {}),
    },
  ]
}

function parseScores(value: unknown): Scores {
  const entry = object(value)
  const rows = (key: string) => list(entry[key]).flatMap(parseScoreRow)
  return { byProvider: rows('by_provider'), byClass: rows('by_class'), byAgent: rows('by_agent'), byLabel: rows('by_label') }
}

function parseRole(value: unknown): RoleRoute[] {
  const entry = object(value)
  const role = text(entry.role)
  if (!(roles as readonly string[]).includes(role)) return []
  const providers = list(entry.providers).filter((p): p is string => typeof p === 'string').map(p => printable(p))
  const upstream = text(entry.upstream)
  return [{ role: role as Role, via: printable(text(entry.via) || 'unknown'), providers, ...(upstream === '' ? {} : { upstream: printable(upstream) }) }]
}

function parseTarget(value: unknown): Target[] {
  const entry = object(value)
  const id = text(entry.id)
  if (id === '' || !id.includes('@')) return []
  return [{ id, label: printable(text(entry.label) || id), provider: printable(text(entry.provider)) }]
}

function parseProvider(value: unknown): ProviderHealth[] {
  const entry = object(value)
  const name = text(entry.name)
  if (name === '') return []
  const limits: Record<string, string> = {}
  for (const [key, raw] of Object.entries(object(entry.rate_limits))) limits[printable(key)] = printable(text(raw))
  return [
    {
      name: printable(name),
      circuit: printable(text(entry.circuit) || 'unknown'),
      attempts: count(entry.attempts),
      answered: count(entry.answered),
      failures: count(entry.failures),
      limits,
      labels: list(entry.labels).filter((l): l is string => typeof l === 'string').map(l => printable(l, 24)),
      auth: printable(text(entry.auth) || 'unknown', 24),
      ready: entry.ready !== false,
    },
  ]
}

function parseSpend(value: unknown): SpendRow[] {
  const entry = object(value)
  const provider = text(entry.provider)
  if (provider === '' && text(entry.model) === '') return []
  const cost = entry.cost
  return [
    {
      provider: printable(provider),
      model: printable(text(entry.model)),
      input: count(entry.input),
      output: count(entry.output),
      cacheRead: count(entry.cache_read),
      requests: count(entry.requests),
      ...(typeof cost === 'number' && Number.isFinite(cost) ? { cost } : {}),
    },
  ]
}

const agentName = /^[A-Za-z0-9_-]{1,64}$/
const toolName = /^[A-Za-z0-9_-]{1,64}$/

function knownTarget(targets: readonly Target[], id: string): boolean {
  return targets.some(item => item.id === id)
}

function parseAgent(value: unknown, targets: readonly Target[]): Agent[] {
  const entry = object(value)
  const name = text(entry.name)
  const target = text(entry.target)
  if (!agentName.test(name) || !knownTarget(targets, target)) return []
  return [
    {
      name,
      target,
      description: printable(text(entry.description), 240),
      prompt: printable(text(entry.prompt), 4000),
      tools: list(entry.tools).filter((t): t is string => typeof t === 'string' && toolName.test(t)).slice(0, 24),
    },
  ]
}

function parseRule(value: unknown, targets: readonly Target[]): SpawnRule[] {
  const entry = object(value)
  const agent = text(entry.agent)
  const target = text(entry.target)
  return agentName.test(agent) && knownTarget(targets, target) ? [{ agent, target }] : []
}

function parseTotals(value: unknown): Totals | undefined {
  const entry = object(value)
  const today = object(entry.today)
  if (Object.keys(today).length === 0) return undefined
  const month = object(entry.month)
  const budget = object(entry.budget)
  const used = (tokens: Record<string, unknown>) => count(tokens.input) + count(tokens.output) + count(tokens.cache_creation)
  return {
    todayTokens: used(today),
    todayRequests: count(today.requests),
    monthTokens: used(month),
    budgetState: printable(text(budget.state) || 'ok'),
    budgetWindow: printable(text(budget.window)),
    budgetPercent: count(budget.percent),
    budgetLimit: count(budget.limit),
  }
}

export function parseOverview(sessionId: string, body: string): Snapshot {
  const empty: Snapshot = { sessionId, roles: [], targets: [], providers: [], spendToday: [], spendMonth: [], agents: [], spawnRules: [], scorecard: noScorecard }
  let document: Record<string, unknown>
  try {
    document = object(JSON.parse(body))
  } catch {
    return { ...empty, error: 'the gateway answered something that is not an overview' }
  }
  if (document.schema_version === undefined) return { ...empty, error: 'the gateway answered something that is not an overview' }
  if (document.schema_version !== overviewVersion) return { ...empty, error: `the gateway's overview has version ${printable(String(document.schema_version))}, this mod reads ${overviewVersion}` }
  const spend = object(document.spend)
  const served = object(document.served)
  return {
    sessionId,
    roles: list(document.roles).flatMap(parseRole),
    targets: list(document.targets).flatMap(parseTarget),
    providers: list(document.providers).flatMap(parseProvider),
    spendToday: list(spend.today).flatMap(parseSpend),
    spendMonth: list(spend.month).flatMap(parseSpend),
    scorecard: { today: parseScores(object(object(document).scorecard).today), month: parseScores(object(object(document).scorecard).month) },
    ...(() => {
      const targets = list(document.targets).flatMap(parseTarget)
      return { agents: list(document.agents).flatMap(entry => parseAgent(entry, targets)), spawnRules: list(document.spawn_rules).flatMap(entry => parseRule(entry, targets)) }
    })(),
    ...(text(served.provider) === ''
      ? {}
      : {
          served: {
            provider: printable(text(served.provider)),
            upstream: printable(text(served.upstream)),
            source: printable(text(served.source)),
            fallback: served.fallback === true,
            switched: served.switched === true,
          },
        }),
    ...(() => {
      const totals = parseTotals(document.totals)
      return totals === undefined ? {} : { totals }
    })(),
  }
}

export function compact(value: number): string {
  if (value >= 1_000_000) return `${(value / 1_000_000).toFixed(1)}M`
  if (value >= 1_000) return `${Math.round(value / 1_000)}k`
  return String(value)
}

const limitLabels: Record<string, string> = {
  'anthropic-ratelimit-unified-5h-utilization': '5h limit',
  'anthropic-ratelimit-unified-7d-utilization': '7d limit',
}

export function describeTotals(found: Snapshot): string {
  const totals = found.totals
  if (totals === undefined) return 'no usage yet'
  const budget =
    totals.budgetWindow === '' ? '' : `  budget ${totals.budgetState} ${totals.budgetPercent}% of ${totals.budgetWindow} ${compact(totals.budgetLimit)}`
  return `today ${compact(totals.todayTokens)} tokens (${totals.todayRequests} req)  month ${compact(totals.monthTokens)}${budget}`
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

export type Command =
  | { kind: 'pane' | 'status' | 'targets' | 'usage' | 'setup' | 'recipes' | 'agents' | 'scorecard' }
  | { kind: 'use'; role: Role; word: string; scope: Scope }
  | { kind: 'error'; message: string }

const subcommands = ['status', 'targets', 'usage', 'use', 'setup', 'recipes', 'agents', 'scorecard', 'pane']

export function parseCommand(args: string): Command {
  const [first = '', ...rest] = args.trim().split(/\s+/).filter(word => word !== '')
  if (first === '' || first === 'pane') return { kind: 'pane' }
  if (first === 'status' || first === 'targets' || first === 'usage' || first === 'setup' || first === 'recipes' || first === 'agents' || first === 'scorecard') return { kind: first }
  if (first !== 'use') return { kind: 'error', message: `unknown subcommand ${printable(first, 32)} (${subcommands.join(', ')})` }
  const [role = '', word = '', flag] = rest
  if (role === '' || word === '') return { kind: 'error', message: 'use ROLE TARGET [default]' }
  if (!(roles as readonly string[]).includes(role)) return { kind: 'error', message: `unknown role ${printable(role, 32)} (${roles.join(', ')})` }
  return { kind: 'use', role: role as Role, word: printable(word, 128), scope: flag === 'default' ? 'global' : 'session' }
}

export type Resolved = { id: string } | { refused: string }

export function resolveTarget(targets: readonly Target[], word: string): Resolved {
  const exact = targets.find(item => item.id === word)
  if (exact !== undefined) return { id: exact.id }
  const named = targets.filter(item => item.id.slice('claude-'.length, item.id.lastIndexOf('@')) === word)
  if (named.length === 1) return { id: named[0].id }
  if (named.length > 1) return { refused: `${word} is offered by more than one provider: ${named.map(item => item.id).join(', ')}` }
  return { refused: `no target ${word} (see /llmgate targets)` }
}

export function statusText(found: Snapshot): string {
  if (found.error) return found.error
  const served = found.served
  if (served === undefined) return 'no reply yet'
  const marker = served.fallback ? ' (fallback)' : served.switched ? ' (switched)' : ''
  return `${served.provider} ${served.upstream}${marker}`
}

function blocked(item: ProviderHealth): string | undefined {
  const status = item.limits['anthropic-ratelimit-unified-status']
  return status !== undefined && status !== 'allowed' ? status : undefined
}

export function changes(before: Snapshot | undefined, after: Snapshot): string[] {
  if (before === undefined || before.error || after.error) return []
  const notes: string[] = []
  const was = before.served
  const now = after.served
  if (now !== undefined && now.fallback && !was?.fallback) notes.push(`llmgate: ${now.provider} answered as a fallback`)
  else if (now !== undefined && was !== undefined && now.provider !== was.provider) notes.push(`llmgate: now served by ${now.provider}`)
  for (const item of after.providers) {
    const prior = before.providers.find(p => p.name === item.name)
    if (prior === undefined) continue
    if (item.circuit === 'open' && prior.circuit !== 'open') notes.push(`llmgate: ${item.name} circuit is open`)
    const status = blocked(item)
    if (status !== undefined && status !== blocked(prior)) notes.push(`llmgate: ${item.name} limit ${status}`)
  }
  return notes
}

export function describeTargets(targets: readonly Target[]): string {
  if (targets.length === 0) return 'no targets offered; add offer to a provider in the llmgate config'
  return targets.map(item => `${item.id}  ${item.label}`).join('\n')
}

export function table(header: readonly string[], rows: readonly (readonly string[])[], right: readonly number[] = []): string[] {
  const widths = header.map((title, at) => Math.max(title.length, ...rows.map(row => (row[at] ?? '').length)))
  const line = (cells: readonly string[]) =>
    cells
      .map((cell, at) => (right.includes(at) ? cell.padStart(widths[at]) : cell.padEnd(widths[at])))
      .join('  ')
      .trimEnd()
  return [line(header), widths.map(width => '-'.repeat(width)).join('  '), ...rows.map(line)]
}

export function routeTable(routes: readonly RoleRoute[]): string[] {
  return table(
    ['role', 'provider', 'model', 'via'],
    routes.map(route => [route.role, route.providers.length === 0 ? 'none' : route.providers.join(', '), route.upstream ?? '-', route.via]),
  )
}

export function spendTable(rows: readonly SpendRow[]): string[] {
  return table(
    ['provider', 'model', 'in', 'out', 'cached', 'req', 'cost'],
    rows.map(row => [row.provider, row.model, compact(row.input), compact(row.output), row.cacheRead > 0 ? compact(row.cacheRead) : '-', String(row.requests), row.cost === undefined ? '-' : `$${row.cost.toFixed(2)}`]),
    [2, 3, 4, 5, 6],
  )
}

export function unpricedNote(rows: readonly SpendRow[]): string | undefined {
  return rows.some(row => row.cost === undefined) ? '- no price: add the model to pricing in the llmgate config' : undefined
}

export function describeProviderLimits(item: ProviderHealth): string {
  const shown = Object.entries(item.limits).flatMap(([key, value]) => {
    const label = limitLabels[key]
    const fraction = Number(value)
    return label !== undefined && Number.isFinite(fraction) ? [`${label} ${Math.round(fraction * 100)}%`] : []
  })
  const status = item.limits['anthropic-ratelimit-unified-status']
  return [...shown, ...(status !== undefined && status !== 'allowed' ? [`limit ${status}`] : [])].join(', ')
}

export function providerTable(items: readonly ProviderHealth[]): string[] {
  return table(
    ['provider', 'labels', 'auth', 'state', 'circuit', 'tried', 'answered', 'failed', 'limits'],
    items.map(item => [
      item.name,
      item.labels.length === 0 ? '-' : item.labels.join(','),
      item.auth,
      item.ready ? 'ready' : 'needs setup',
      item.circuit,
      String(item.attempts),
      String(item.answered),
      String(item.failures),
      describeProviderLimits(item) || '-',
    ]),
    [5, 6, 7],
  )
}

export function agentTable(found: Snapshot): string[] {
  const provider = (id: string) => found.targets.find(item => item.id === id)?.provider ?? '-'
  const rows: string[][] = [
    ...found.roles.map(route => ['role', route.role, route.providers.join(', ') || 'none', route.providers.join(', ') || '-']),
    ...found.agents.map(item => ['agent type', item.name, item.target, provider(item.target)]),
    ...found.spawnRules.map(item => ['spawn rule', item.agent, item.target, provider(item.target)]),
  ]
  return table(['kind', 'name', 'runs on', 'provider'], rows)
}
