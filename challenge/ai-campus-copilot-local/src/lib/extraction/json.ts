import type { ZodType, ZodTypeDef } from "zod";

/**
 * Pulls the first balanced JSON array or object out of a model reply. Local
 * models frequently wrap JSON in prose or ``` fences, so a bare JSON.parse of
 * the whole reply is not reliable enough.
 */
export function extractJsonBlock(reply: string): string | null {
  const withoutFences = reply.replace(/```(?:json)?/gi, "```");
  const source = withoutFences.includes("```")
    ? (withoutFences.split("```").find((part) => /[[{]/.test(part)) ?? withoutFences)
    : withoutFences;

  const start = firstIndexOfAny(source, ["[", "{"]);
  if (start === -1) return null;

  const open = source[start] as "[" | "{";
  const close = open === "[" ? "]" : "}";

  let depth = 0;
  let inString = false;
  let escaped = false;

  for (let i = start; i < source.length; i += 1) {
    const char = source[i] as string;

    if (escaped) {
      escaped = false;
      continue;
    }
    if (char === "\\") {
      escaped = true;
      continue;
    }
    if (char === '"') {
      inString = !inString;
      continue;
    }
    if (inString) continue;

    if (char === open) depth += 1;
    else if (char === close) {
      depth -= 1;
      if (depth === 0) return source.slice(start, i + 1);
    }
  }

  return null;
}

export interface ParseResult<T> {
  ok: boolean;
  data: T | null;
  error: string | null;
}

/** Extracts, parses and schema-validates a model reply in one step. */
export function parseModelJson<T>(
  reply: string,
  schema: ZodType<T, ZodTypeDef, unknown>,
): ParseResult<T> {
  const block = extractJsonBlock(reply);
  if (!block) {
    return { ok: false, data: null, error: "The model reply contained no JSON." };
  }

  let raw: unknown;
  try {
    raw = JSON.parse(block);
  } catch (cause) {
    return {
      ok: false,
      data: null,
      error: `The model returned malformed JSON: ${cause instanceof Error ? cause.message : "parse error"}`,
    };
  }

  const parsed = schema.safeParse(raw);
  if (!parsed.success) {
    const issue = parsed.error.issues[0];
    return {
      ok: false,
      data: null,
      error: `The model output did not match the expected schema${
        issue ? `: ${issue.path.join(".")} ${issue.message}` : "."
      }`,
    };
  }

  return { ok: true, data: parsed.data, error: null };
}

function firstIndexOfAny(text: string, needles: string[]): number {
  let best = -1;
  for (const needle of needles) {
    const index = text.indexOf(needle);
    if (index !== -1 && (best === -1 || index < best)) best = index;
  }
  return best;
}
