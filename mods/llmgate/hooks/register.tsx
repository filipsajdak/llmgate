import type { EngineInterface as Engine, Register } from 'claude-code'

import { describeProvider, describeRoute, describeSpend, describeTotals, gatewayRoot, invocation, parseOverview, printable, roles } from './model'
import type { Change, Role, Scope, Snapshot } from './model'

const PANE = 'llmgate'
const pollMilliseconds = 5000
const unreadable: Snapshot = { sessionId: '', roles: [], targets: [], providers: [], spendToday: [], spendMonth: [] }

type View = 'now' | 'usage'

let snapshot: Snapshot = unreadable
let selected: Role = 'sonnet'
let view: View = 'now'
let message = ''
let isOpen = false

async function load($: Engine): Promise<Snapshot> {
  const root = gatewayRoot(await $.env.get('ANTHROPIC_BASE_URL'))
  if (root === undefined) return { ...unreadable, error: 'no llmgate gateway: start Claude Code with llmgate claude' }
  const token = await $.env.get('ANTHROPIC_AUTH_TOKEN')
  const init = token === undefined ? {} : { headers: { authorization: `Bearer ${token}` } }
  const sessionId = String(await $.session.id())
  try {
    const answer = await $.http.fetch(`${root}/llmgate/overview?session=${encodeURIComponent(sessionId)}`, init)
    if (!answer.ok) return { ...unreadable, sessionId, error: `the gateway answered ${answer.status}` }
    return parseOverview(sessionId, answer.text)
  } catch (error) {
    return { ...unreadable, sessionId, error: printable(`the gateway did not answer: ${String(error)}`) }
  }
}

async function refresh($: Engine): Promise<void> {
  snapshot = await load($)
  $.ui.invalidate('ui.render')
}

async function change($: Engine, what: Change): Promise<void> {
  const [bin, config] = await Promise.all([$.env.get('LLMGATE_BIN'), $.env.get('LLMGATE_CONFIG')])
  const plan = invocation(bin, config, snapshot.sessionId, snapshot.targets, what)
  if ('refused' in plan) {
    message = plan.refused
  } else {
    try {
      const ran = await $.process.run(plan.argv)
      message = ran.exitCode === 0 ? done(what) : printable(`llmgate: ${ran.stderr.trim() || `exit ${ran.exitCode}`}`)
    } catch (error) {
      message = printable(`llmgate did not run: ${String(error)}`)
    }
  }
  await refresh($)
}

function done(what: Change): string {
  const where = what.scope === 'session' ? 'for this session' : 'for every session'
  return 'clear' in what ? `${what.role} override removed ${where}` : `${what.role} now uses ${what.target} ${where}`
}

function summary(): string {
  if (snapshot.error) return snapshot.error
  return [...snapshot.roles.map(describeRoute), describeTotals(snapshot)].join('\n')
}

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    await $.command.register({ name: 'llmgate', description: 'Show and switch the llmgate route of this session', immediate: true })
    $.clock.every(pollMilliseconds, () => {
      if (isOpen) void refresh($)
    })
    return next(e)
  })

  on('command.run', { command: 'llmgate' }, async $ => {
    await refresh($)
    const opened = await $.ui.open({ id: PANE, title: 'llmgate', focus: true, closeOnEscape: true })
    isOpen = opened.isPlaced
    return opened.isPlaced ? {} : { text: summary() }
  })

  on('ui.close', async ($, e, next) => {
    if (e.id === PANE) isOpen = false
    return next(e)
  })

  on('ui.render', { component: 'Pane', requestId: PANE }, async ($, e) => {
    const { Box, Text, Button } = $.ui.resolve(e)
    const redraw = () => $.ui.invalidate('ui.render')
    const apply = (scope: Scope, target: string) => () => void change($, { role: selected, target, scope })
    const show = (next: View) => () => {
      view = next
      redraw()
    }

    const header = (
      <Box flexDirection="column">
        <Box flexDirection="row" columnGap={2}>
          <Text bold>llmgate {snapshot.sessionId ? snapshot.sessionId.slice(0, 8) : ''}</Text>
          <Button key="view-now" label="Now" hotkey="n" plain dimColor={view !== 'now'} onPress={show('now')} />
          <Button key="view-usage" label="Usage" hotkey="u" plain dimColor={view !== 'usage'} onPress={show('usage')} />
          <Button key="refresh" label="refresh" hotkey="r" plain onPress={() => void refresh($)} />
        </Box>
        {snapshot.error && <Text color="red">{snapshot.error}</Text>}
      </Box>
    )

    if (view === 'usage') {
      return (
        <Box flexDirection="column">
          {header}
          <Text bold>{describeTotals(snapshot)}</Text>
          <Text> </Text>
          <Text bold>Today</Text>
          {snapshot.spendToday.length === 0 && <Text dimColor>no requests yet today</Text>}
          {snapshot.spendToday.map(row => (
            <Text>{describeSpend(row)}</Text>
          ))}
          <Text> </Text>
          <Text bold>This month</Text>
          {snapshot.spendMonth.length === 0 && <Text dimColor>no requests this month</Text>}
          {snapshot.spendMonth.map(row => (
            <Text>{describeSpend(row)}</Text>
          ))}
          <Text> </Text>
          <Text bold>Providers</Text>
          {snapshot.providers.map(item => (
            <Text>{describeProvider(item)}</Text>
          ))}
        </Box>
      )
    }

    return (
      <Box flexDirection="column">
        {header}
        {snapshot.served && (
          <Text>
            last reply: {snapshot.served.provider} {snapshot.served.upstream}
            {snapshot.served.fallback ? ' (fallback)' : ''}
            {snapshot.served.switched ? ' (switched)' : ''}
          </Text>
        )}
        {snapshot.roles.map(route => (
          <Text dimColor={route.role !== selected}>{describeRoute(route)}</Text>
        ))}
        <Text> </Text>
        <Box flexDirection="row" columnGap={2}>
          {roles.map((role, at) => (
            <Button
              key={`role-${role}`}
              label={role}
              hotkey={String(at + 1)}
              plain
              dimColor={role !== selected}
              onPress={() => {
                selected = role
                redraw()
              }}
            />
          ))}
        </Box>
        {snapshot.targets.length === 0 && <Text dimColor>No targets offered; add offer to a provider in the llmgate config.</Text>}
        {snapshot.targets.map(target => (
          <Box flexDirection="row" columnGap={2}>
            <Button key={`use-${target.id}`} label={`use for ${selected}`} onPress={apply('session', target.id)} />
            <Button key={`default-${target.id}`} label="make default" onPress={apply('global', target.id)} />
            <Text>{target.label}</Text>
          </Box>
        ))}
        <Button key="clear" label="clear session override" hotkey="c" onPress={() => void change($, { role: selected, clear: true, scope: 'session' })} />
        {message && <Text dimColor>{message}</Text>}
      </Box>
    )
  })
}
