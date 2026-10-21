import type { EngineInterface as Engine, Register } from 'claude-code'

import { describeRoute, gatewayRoot, invocation, parseRoute, parseServed, parseTargets, printable, roles } from './model'
import type { Change, Role, Scope, Snapshot } from './model'

const PANE = 'llmgate'
const pollMilliseconds = 5000
const unreadable: Snapshot = { sessionId: '', roles: [], targets: [] }

let snapshot: Snapshot = unreadable
let selected: Role = 'sonnet'
let message = ''
let isOpen = false

async function load($: Engine): Promise<Snapshot> {
  const root = gatewayRoot(await $.env.get('ANTHROPIC_BASE_URL'))
  if (root === undefined) return { ...unreadable, error: 'no llmgate gateway: start Claude Code with llmgate claude' }
  const token = await $.env.get('ANTHROPIC_AUTH_TOKEN')
  const init = token === undefined ? {} : { headers: { authorization: `Bearer ${token}` } }
  const sessionId = String(await $.session.id())
  const encoded = encodeURIComponent(sessionId)
  try {
    const [models, served, ...routes] = await Promise.all([
      $.http.fetch(`${root}/anthropic/v1/models`, init),
      $.http.fetch(`${root}/llmgate/session?id=${encoded}`, init),
      ...roles.map(role => $.http.fetch(`${root}/llmgate/route?requested=${role}&session=${encoded}`, init)),
    ])
    if (!models.ok) return { ...unreadable, sessionId, error: `the gateway answered ${models.status}` }
    return {
      sessionId,
      targets: parseTargets(models.text),
      served: parseServed(served.text),
      roles: roles.map((role, at) => parseRoute(role, routes[at].text)),
    }
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
  return snapshot.roles.map(describeRoute).join('\n')
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
    const here = snapshot.targets.filter(item => item.id !== '')

    return (
      <Box flexDirection="column">
        <Text bold>llmgate {snapshot.sessionId ? `session ${snapshot.sessionId.slice(0, 8)}` : ''}</Text>
        {snapshot.error && <Text color="red">{snapshot.error}</Text>}
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
        {here.length === 0 && <Text dimColor>No targets offered; add offer to a provider in the llmgate config.</Text>}
        {here.map(target => (
          <Box flexDirection="row" columnGap={2}>
            <Button key={`use-${target.id}`} label={`use for ${selected}`} onPress={apply('session', target.id)} />
            <Button key={`default-${target.id}`} label="make default" onPress={apply('global', target.id)} />
            <Text>{target.label}</Text>
          </Box>
        ))}
        <Box flexDirection="row" columnGap={2}>
          <Button key="clear" label="clear session override" hotkey="c" onPress={() => void change($, { role: selected, clear: true, scope: 'session' })} />
          <Button key="refresh" label="refresh" hotkey="r" onPress={() => void refresh($)} />
        </Box>
        {message && <Text dimColor>{message}</Text>}
      </Box>
    )
  })
}
