import { expect, test } from 'claude-code/testing'

import { parseRecipes, recipeInvocation } from '../hooks/recipes'

const listing = JSON.stringify([
  { id: 'quality-first', title: 'Quality first, with fallback', summary: 'Claude answers every role.', pros: ['one vendor'], cons: ['spends Claude quota'], needs: ['claude'], tokens: '1x' },
  { id: 'spend-evenly', title: 'Spend the subscriptions evenly', summary: 'Opus on Claude.', pros: [], cons: ['three sign-ins'], needs: ['claude', 'chatgpt', 'copilot'], tokens: '1x' },
])

test('the recipe list is read with plain fields and refuses what is not a list', () => {
  const found = parseRecipes(listing)
  expect(found.map(r => r.id)).toEqual(['quality-first', 'spend-evenly'])
  expect(found[1].needs).toEqual(['claude', 'chatgpt', 'copilot'])
  expect(parseRecipes('not json')).toEqual([])
  expect(parseRecipes('{"a":1}')).toEqual([])
  expect(parseRecipes(JSON.stringify([{ id: 'bad id!' }, { id: 'ok', title: 't' }])).map(r => r.id)).toEqual(['ok'])
  expect(parseRecipes(JSON.stringify([{ id: 'x', title: 'a\u001b[2Jb', summary: '', pros: [], cons: [], needs: [], tokens: '' }]))[0].title).toBe('a?[2Jb')
})

test('a recipe runs by argv from the loaded list only, previewing without --yes', () => {
  const found = parseRecipes(listing)
  expect(recipeInvocation('/opt/llmgate', '/c.jsonc', found, 'quality-first', false)).toEqual({ argv: ['/opt/llmgate', '--config', '/c.jsonc', 'recipe', 'apply', 'quality-first'] })
  expect(recipeInvocation('/opt/llmgate', undefined, found, 'quality-first', true)).toEqual({ argv: ['/opt/llmgate', 'recipe', 'apply', 'quality-first', '--yes'] })
  expect(recipeInvocation('/opt/llmgate', '/c.jsonc', found, 'unknown', false)).toEqual({ refused: 'that recipe is not in the list' })
  expect(recipeInvocation(undefined, '/c.jsonc', found, 'quality-first', false)).toEqual({ refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' })
})
