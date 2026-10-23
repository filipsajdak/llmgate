import { expect, test } from 'claude-code/testing'

import { parseOverview } from '../hooks/model'
import { meterLines, parseQuota, scoreTable } from '../hooks/scorecard'

const row = (name: string, extra: object = {}) => ({ name, input: 1200, output: 300, cache_read: 3600, cache_creation: 0, requests: 4, average_ms: 250, max_ms: 900, failures: 1, fallbacks: 2, share_percent: 80, ...extra })

const found = parseOverview(
  's',
  JSON.stringify({
    schema_version: 1,
    roles: [],
    targets: [],
    providers: [
      { name: 'anthropic', attempts: 3, answered: 3, failures: 0, circuit: 'closed', rate_limits: { 'anthropic-ratelimit-unified-5h-utilization': '0.13', 'anthropic-ratelimit-unified-7d-utilization': '0.4' }, labels: [], auth: 'forward', ready: true },
      { name: 'codex', attempts: 1, answered: 1, failures: 0, circuit: 'closed', rate_limits: {}, labels: [], auth: 'chatgpt-login', ready: true },
      { name: 'work', attempts: 1, answered: 1, failures: 0, circuit: 'closed', rate_limits: {}, labels: [], auth: 'copilot-login', ready: true },
    ],
    spend: { today: [], month: [] },
    scorecard: {
      today: { by_provider: [row('codex', { cost: 0.5 }), row('work')], by_class: [row('subagent')], by_agent: [row('custom')], by_label: [row('work')] },
      month: { by_provider: [], by_class: [], by_agent: [], by_label: [] },
    },
  }),
)

test('the scorecard is read into rows with plain numbers and an optional cost', () => {
  expect(found.scorecard.today.byProvider.map(r => r.name)).toEqual(['codex', 'work'])
  expect(found.scorecard.today.byProvider[0]).toEqual({ name: 'codex', requests: 4, input: 1200, output: 300, cacheRead: 3600, averageMs: 250, maxMs: 900, failures: 1, fallbacks: 2, share: 80, cost: 0.5 })
  expect(found.scorecard.today.byProvider[1].cost).toBeUndefined()
  expect(found.scorecard.month.byProvider).toEqual([])
})

test('a score table lines up the columns, shows the cache share and a dash for an unknown price', () => {
  expect(scoreTable(found.scorecard.today.byProvider)).toEqual([
    'name   req  share  in  out  cached   cost    avg    max  fail  fb',
    '-----  ---  -----  --  ---  ------  -----  -----  -----  ----  --',
    'codex    4    80%  1k  300     75%  $0.50  250ms  900ms     1   2',
    'work     4    80%  1k  300     75%      -  250ms  900ms     1   2',
  ])
  expect(scoreTable([])).toEqual([])
})

test('the quota summary of a Copilot login is read and a bad one is refused', () => {
  const summary = parseQuota(JSON.stringify({ plan: 'business', sku: 'x', reset_date: '2026-11-01', quotas: [{ name: 'premium_interactions', unlimited: false, has_quota: true, entitlement: 300, remaining: 120, percent_remaining: 40 }, { name: 'chat', unlimited: true, has_quota: false, entitlement: 0, remaining: 0, percent_remaining: 100 }] }))
  expect(summary).toEqual({ plan: 'business', reset: '2026-11-01', lines: ['premium_interactions 120 of 300 left (40%)', 'chat unlimited'] })
  expect(parseQuota('nope')).toBeUndefined()
  expect(parseQuota(JSON.stringify({ plan: 1 }))).toBeUndefined()
})

test('the meters say what each provider reports and admit what the route does not', () => {
  const quotas = new Map([['work', { plan: 'business', reset: '2026-11-01', lines: ['premium_interactions 120 of 300 left (40%)'] }]])
  expect(meterLines(found, quotas)).toEqual([
    'anthropic  5h limit 13%, 7d limit 40%',
    'codex      no quota is reported by this route; 2k tokens today',
    'work       business, resets 2026-11-01: premium_interactions 120 of 300 left (40%)',
  ])
})
