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
