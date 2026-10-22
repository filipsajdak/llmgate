import { printable } from './model'

export type Recipe = { id: string; title: string; summary: string; pros: string[]; cons: string[]; needs: string[]; tokens: string }

const idPattern = /^[a-z0-9-]{1,40}$/

function words(value: unknown): string[] {
  return Array.isArray(value) ? value.filter((item): item is string => typeof item === 'string').map(item => printable(item, 160)) : []
}

export function parseRecipes(body: string): Recipe[] {
  let document: unknown
  try {
    document = JSON.parse(body)
  } catch {
    return []
  }
  if (!Array.isArray(document)) return []
  return document.flatMap((entry: unknown) => {
    const item = typeof entry === 'object' && entry !== null ? (entry as Record<string, unknown>) : {}
    const id = typeof item.id === 'string' ? item.id : ''
    if (!idPattern.test(id)) return []
    return [
      {
        id,
        title: printable(typeof item.title === 'string' ? item.title : id, 80),
        summary: printable(typeof item.summary === 'string' ? item.summary : '', 240),
        pros: words(item.pros),
        cons: words(item.cons),
        needs: words(item.needs),
        tokens: printable(typeof item.tokens === 'string' ? item.tokens : '', 24),
      },
    ]
  })
}

export type Plan = { argv: string[] } | { refused: string }

export function recipeInvocation(bin: string | undefined, config: string | undefined, known: readonly Recipe[], id: string, write: boolean): Plan {
  if (bin === undefined || bin === '') return { refused: 'LLMGATE_BIN is not set: start Claude Code with llmgate claude' }
  if (!known.some(item => item.id === id)) return { refused: 'that recipe is not in the list' }
  return { argv: [bin, ...(config ? ['--config', config] : []), 'recipe', 'apply', id, ...(write ? ['--yes'] : [])] }
}
