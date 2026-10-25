import { compact, describeProviderLimits, printable, table } from './model'
import type { ScoreRow, Snapshot } from './model'

export function scoreTable(rows: readonly ScoreRow[]): string[] {
  if (rows.length === 0) return []
  return table(
    ['name', 'req', 'share', 'in', 'out', 'cached', 'cost', 'avg', 'max', 'fail', 'fb'],
    rows.map(item => {
      const seen = item.input + item.cacheRead
      return [
        item.name,
        String(item.requests),
        `${item.share}%`,
        compact(item.input),
        compact(item.output),
        seen === 0 ? '-' : `${Math.round((item.cacheRead * 100) / seen)}%`,
        item.cost === undefined ? '-' : `$${item.cost.toFixed(2)}`,
        `${item.averageMs}ms`,
        `${item.maxMs}ms`,
        String(item.failures),
        String(item.fallbacks),
      ]
    }),
    [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
  )
}

export type Quota = { plan: string; reset: string; lines: string[] }

export function parseQuota(body: string): Quota | undefined {
  let document: unknown
  try {
    document = JSON.parse(body)
  } catch {
    return undefined
  }
  const entry = typeof document === 'object' && document !== null ? (document as Record<string, unknown>) : {}
  if (typeof entry.plan !== 'string' || !Array.isArray(entry.quotas)) return undefined
  const lines = entry.quotas.flatMap((raw: unknown) => {
    const item = typeof raw === 'object' && raw !== null ? (raw as Record<string, unknown>) : {}
    const name = printable(typeof item.name === 'string' ? item.name : '', 40)
    if (name === '') return []
    if (item.unlimited === true) return [`${name} unlimited`]
    if (item.has_quota !== true) return []
    return [`${name} ${Number(item.remaining) || 0} of ${Number(item.entitlement) || 0} left (${Number(item.percent_remaining) || 0}%)`]
  })
  return { plan: printable(entry.plan, 40), reset: printable(typeof entry.reset_date === 'string' ? entry.reset_date : '', 24), lines }
}

export function meterLines(found: Snapshot, quotas: ReadonlyMap<string, Quota>): string[] {
  const width = Math.max(0, ...found.providers.map(item => item.name.length))
  const today = new Map(found.scorecard.today.byProvider.map(item => [item.name, item.input + item.output]))
  return found.providers.map(item => {
    const name = item.name.padEnd(width)
    const quota = quotas.get(item.name)
    const limits = describeProviderLimits(item)
    if (quota !== undefined) return `${name}  ${quota.plan}${quota.reset === '' ? '' : `, resets ${quota.reset}`}: ${quota.lines.join('; ')}`
    if (limits !== '') return `${name}  ${limits}`
    return `${name}  no quota is reported by this route; ${compact(today.get(item.name) ?? 0)} tokens today`
  })
}

export type CheckRow = { provider: string; step: string; result: 'pass' | 'fail' | 'skipped'; detail: string; milliseconds: number }

export function parseCheck(body: string): CheckRow[] {
  let document: unknown
  try {
    document = JSON.parse(body)
  } catch {
    return []
  }
  if (!Array.isArray(document)) return []
  return document.flatMap((raw: unknown) => {
    const item = typeof raw === 'object' && raw !== null ? (raw as Record<string, unknown>) : {}
    const result = item.result === 'pass' || item.result === 'fail' || item.result === 'skipped' ? item.result : undefined
    if (result === undefined || typeof item.provider !== 'string') return []
    return [
      {
        provider: printable(item.provider, 48),
        step: printable(typeof item.step === 'string' ? item.step : '', 16),
        result,
        detail: printable(typeof item.detail === 'string' ? item.detail : '', 120),
        milliseconds: Number(item.milliseconds) || 0,
      },
    ]
  })
}

export type Plan = { argv: string[] } | { refused: string }

export function checkInvocation(bin: string | undefined, config: string | undefined, quick: boolean, provider?: string): Plan {
  if (bin === undefined || bin === '') return { refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' }
  return { argv: [bin, ...(config ? ['--config', config] : []), 'check', '--json', ...(quick ? ['--quick'] : []), ...(provider === undefined ? [] : ['--provider', provider])] }
}

export function checkWarning(quick: boolean, providers: readonly string[]): string {
  const where = providers.join(', ')
  return quick
    ? `The quick check sends one small request to each provider (${where}). Press Run to go on.`
    : `The full check sends one small request, a tool request and one of about 16k tokens to each provider (${where}), and one small request to the target of each agent, and uses that much of their quota. Press Run to go on.`
}

export function checkTable(rows: readonly CheckRow[]): string[] {
  return table(
    ['provider', 'step', 'result', 'time', 'detail'],
    rows.map(row => [row.provider, row.step, row.result === 'pass' ? 'pass' : row.result === 'fail' ? 'FAIL' : 'skip', row.result === 'skipped' ? '-' : row.milliseconds < 1000 ? `${row.milliseconds} ms` : `${(row.milliseconds / 1000).toFixed(1)} s`, row.detail]),
    [3],
  )
}
