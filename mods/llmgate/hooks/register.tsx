import type { EngineInterface as Engine, Register } from 'claude-code'

import { addInvocation, followUp, formFor, kinds, nextStep } from './setup'
import type { Form, KindId } from './setup'
import { changes, describeTargets, describeTotals, providerTable, routeTable, spendTable, unpricedNote, gatewayRoot, invocation, parseCommand, parseOverview, printable, printableBlock, resolveTarget, roles, statusText } from './model'
import type { Change, Command, Role, Scope, Snapshot } from './model'

const PANE = 'llmgate'
const pollMilliseconds = 5000
const unreadable: Snapshot = { sessionId: '', roles: [], targets: [], providers: [], spendToday: [], spendMonth: [] }

type View = 'now' | 'usage' | 'setup'

let snapshot: Snapshot = unreadable
let selected: Role = 'sonnet'
let view: View = 'now'
let message = ''
let isOpen = false
let hasRead = false
let form: Form | undefined
let preview = ''
let next = ''

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
  const before = hasRead ? snapshot : undefined
  snapshot = await load($)
  hasRead = true
  await $.ui.status(statusText(snapshot))
  for (const note of changes(before, snapshot)) await $.ui.toast(note)
  $.ui.invalidate('ui.render')
}

async function change($: Engine, what: Change): Promise<string> {
  const [bin, config] = await Promise.all([$.env.get('LLMGATE_BIN'), $.env.get('LLMGATE_CONFIG')])
  const plan = invocation(bin, config, snapshot.sessionId, snapshot.targets, what)
  if ('refused' in plan) {
    message = plan.refused
  } else {
    try {
      const ran = await $.process.run(plan.argv)
      message = ran.exitCode === 0 ? done(what) : printable(`llmgate: ${ran.stderr.trim().replace(/^llmgate: /, '') || `exit ${ran.exitCode}`}`)
    } catch (error) {
      message = printable(`llmgate did not run: ${String(error)}`)
    }
  }
  await refresh($)
  return message
}

function setField(key: keyof Form, value: string): void {
  if (form !== undefined) form = { ...form, [key]: value }
}

async function runAdd($: Engine, write: boolean): Promise<void> {
  if (form === undefined) return
  const [bin, config] = await Promise.all([$.env.get('LLMGATE_BIN'), $.env.get('LLMGATE_CONFIG')])
  const plan = addInvocation(bin, config, form, write)
  if ('refused' in plan) {
    preview = plan.refused
    next = ''
  } else {
    try {
      const ran = await $.process.run(plan.argv)
      const out = printableBlock((ran.exitCode === 0 ? ran.stdout : ran.stderr.replace(/^llmgate: /, '') || `exit ${ran.exitCode}`).trim())
      preview = out
      if (write && ran.exitCode === 0) {
        const more = followUp(bin as string, config, form)
        if (more !== undefined) await $.process.run(more)
      }
      next = write && ran.exitCode === 0 ? nextStep(bin as string, config, form) : ''
    } catch (error) {
      preview = printable(`llmgate did not run: ${String(error)}`)
      next = ''
    }
  }
  if (write) await refresh($)
  $.ui.invalidate('ui.render')
}

function done(what: Change): string {
  const where = what.scope === 'session' ? 'for this session' : 'for every session'
  return 'clear' in what ? `${what.role} override removed ${where}` : `${what.role} now uses ${what.target} ${where}`
}

function summary(): string {
  if (snapshot.error) return snapshot.error
  return [...routeTable(snapshot.roles), '', describeTotals(snapshot)].join('\n')
}

function usageText(): string[] {
  const note = unpricedNote([...snapshot.spendToday, ...snapshot.spendMonth])
  return [
    describeTotals(snapshot),
    '',
    'Today',
    ...(snapshot.spendToday.length === 0 ? ['no requests yet today'] : spendTable(snapshot.spendToday)),
    '',
    'This month',
    ...(snapshot.spendMonth.length === 0 ? ['no requests this month'] : spendTable(snapshot.spendMonth)),
    ...(note === undefined ? [] : ['', note]),
    '',
    'Providers',
    ...providerTable(snapshot.providers),
  ]
}

async function answer($: Engine, command: Command): Promise<string> {
  if (command.kind === 'error') return command.message
  if (snapshot.error) return snapshot.error
  if (command.kind === 'targets') return describeTargets(snapshot.targets)
  if (command.kind === 'usage') return usageText().join('\n')
  if (command.kind === 'use') return useTarget($, command)
  return [summary(), statusText(snapshot)].join('\n')
}

async function useTarget($: Engine, command: Extract<Command, { kind: 'use' }>): Promise<string> {
  if (command.word === 'clear') return change($, { role: command.role, clear: true, scope: command.scope })
  const target = resolveTarget(snapshot.targets, command.word)
  if ('refused' in target) return target.refused
  return change($, { role: command.role, target: target.id, scope: command.scope })
}

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    await $.command.register({ name: 'llmgate', description: 'Show and switch the llmgate route of this session', immediate: true })
    $.clock.every(pollMilliseconds, () => void refresh($))
    return next(e)
  })

  on('command.run', { command: 'llmgate' }, async ($, e) => {
    await refresh($)
    const command = parseCommand(e.args)
    if (command.kind === 'setup') view = 'setup'
    else if (command.kind !== 'pane') return { text: await answer($, command) }
    const opened = await $.ui.open({ id: PANE, title: 'llmgate', focus: true, closeOnEscape: true })
    isOpen = opened.isPlaced
    return opened.isPlaced ? {} : { text: summary() }
  })

  on('ui.close', async ($, e, next) => {
    if (e.id === PANE) isOpen = false
    return next(e)
  })

  on('ui.render', { component: 'Pane', requestId: PANE }, async ($, e) => {
    const { Box, Text, Button, Input } = $.ui.resolve(e)
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
          <Button key="view-setup" label="Setup" hotkey="s" plain dimColor={view !== 'setup'} onPress={show('setup')} />
          <Button key="refresh" label="refresh" hotkey="r" plain onPress={() => void refresh($)} />
        </Box>
        {snapshot.error && <Text color="red">{snapshot.error}</Text>}
      </Box>
    )

    const lines = (rows: string[]) =>
      rows.map((row, at) => (
        <Text bold={at === 0} dimColor={at === 1}>
          {row}
        </Text>
      ))

    if (view === 'setup') {
      const choose = (id: KindId) => () => {
        form = formFor(id)
        preview = ''
        next = ''
        redraw()
      }
      const field = (key: keyof Form, label: string, placeholder: string) => (
        <Input
          key={`field-${key}`}
          label={label}
          placeholder={placeholder}
          value={form === undefined ? '' : String(form[key])}
          onInput={value => setField(key, value)}
          onSubmit={value => {
            setField(key, value)
            void runAdd($, false)
          }}
        />
      )
      const kind = kinds.find(item => item.id === form?.kind)
      return (
        <Box flexDirection="column">
          {header}
          <Text bold>Providers</Text>
          {lines(providerTable(snapshot.providers))}
          <Text> </Text>
          <Text bold>Add a provider: what kind is it?</Text>
          <Box flexDirection="row" columnGap={2}>
            {kinds.map(item => (
              <Button key={`kind-${item.id}`} label={item.title} plain dimColor={form?.kind !== item.id} onPress={choose(item.id)} />
            ))}
          </Box>
          {kind !== undefined && form !== undefined && (
            <Box flexDirection="column">
              <Text>{kind.help}</Text>
              {field('name', 'name', 'letters, digits, - and _')}
              {(form.kind === 'apikey' || form.kind === 'local' || form.kind === 'gateway') && field('baseUrl', 'address', 'https://host or http://127.0.0.1:port')}
              {form.kind === 'apikey' && field('header', 'header', 'api-key')}
              {form.kind === 'apikey' && field('scheme', 'scheme', 'optional, such as Bearer')}
              {form.kind === 'copilot' && field('gheHost', 'GitHub host', 'optional, such as corp.ghe.example')}
              {(form.kind === 'apikey' || form.kind === 'local' || form.kind === 'gateway') && field('offer', 'models', 'comma separated names to list in /model')}
              {field('labels', 'labels', 'optional, such as work,acme')}
              <Box flexDirection="row" columnGap={2}>
                <Button key="preview" label="Preview" onPress={() => void runAdd($, false)} />
                <Button key="apply" label="Apply" onPress={() => void runAdd($, true)} />
              </Box>
              {form.kind === 'apikey' && <Text dimColor>The key is never typed here: you enter it hidden in the terminal, next.</Text>}
              {preview !== '' && preview.split('\n').map(row => <Text>{row === '' ? ' ' : row}</Text>)}
              {next !== '' && <Text bold>Next, type this in the prompt:</Text>}
              {next !== '' && <Text>{next}</Text>}
            </Box>
          )}
        </Box>
      )
    }

    if (view === 'usage') {
      const note = unpricedNote([...snapshot.spendToday, ...snapshot.spendMonth])
      return (
        <Box flexDirection="column">
          {header}
          <Text bold>{describeTotals(snapshot)}</Text>
          <Text> </Text>
          <Text bold>Today</Text>
          {snapshot.spendToday.length === 0 ? <Text dimColor>no requests yet today</Text> : lines(spendTable(snapshot.spendToday))}
          <Text> </Text>
          <Text bold>This month</Text>
          {snapshot.spendMonth.length === 0 ? <Text dimColor>no requests this month</Text> : lines(spendTable(snapshot.spendMonth))}
          {note && <Text dimColor>{note}</Text>}
          <Text> </Text>
          <Text bold>Providers</Text>
          {lines(providerTable(snapshot.providers))}
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
        {routeTable(snapshot.roles).map((row, at) => (
          <Text bold={at === 0} dimColor={at === 1 || (at > 1 && snapshot.roles[at - 2].role !== selected)}>
            {row}
          </Text>
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
