// Provider-agnostic contract for the LLM text-generation layer.
// Providers implement TextProvider; the HTTP route only ever talks to this
// interface.
//
// Deliberately JSON-free: models (especially small/free ones) fail JSON
// envelopes in many ways (structured-output rejection, prose wrapping,
// truncated objects, missing keys). Instead the model returns plain markdown
// with two machine-readable conventions:
//   - [Term](Term) links inline in the text (the branching mechanism),
//   - a trailing "## Explore further" section of bullet links (the prompts).
// Both degrade gracefully: missing links -> text-only node, missing section ->
// node without prompts. Nothing to fail hard.

export interface TextProvider {
  generateText(prompt: string): Promise<{ text: string; prompts: string[] }>;
}

/** Error carrying the upstream HTTP status so routes can surface it. */
export class ProviderError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

export const SYSTEM_PROMPT = `You are an infinite spatial-knowledge-engine generator. Respond with markdown text about the user's topic: brief but impactful explanatory text. CRITICAL: wrap 2 to 4 key concepts or interesting terms as clickable markdown links in the exact format [Term](Term) with no spaces, for example 'Machine learning relies on [Supervised Learning](Supervised Learning) and [Neural Networks](Neural Networks).' Do not use bold or italics for those terms. End your response with a section headed exactly '## Explore further' containing exactly 3 bullet-point markdown links, each bullet like '- [A follow-up question](A follow-up question)'. Output only the markdown; no JSON, no code fences.`;

/** Splits markdown output into display text and follow-up prompts. */
export function parseMarkdownContent(raw: string): { text: string; prompts: string[] } {
  const trimmed = raw.trim();
  const heading = trimmed.match(/^##\s+explore further\s*$/im);
  if (!heading || heading.index === undefined) {
    return { text: trimmed, prompts: [] };
  }
  const text = trimmed.slice(0, heading.index).trim();
  const section = trimmed.slice(heading.index + heading[0].length);
  const prompts = [...section.matchAll(/^\s*[-*]\s+\[([^\]]+)\]\([^)]*\)\s*$/gm)]
    .map((m) => m[1].trim())
    .slice(0, 3);
  return { text, prompts };
}

/** Shared OpenAI-compatible chat-completions caller used by the providers. */
export async function callChatCompletions(options: {
  baseUrl: string;
  apiKey: string | undefined;
  model: string;
  prompt: string;
}): Promise<{ text: string; prompts: string[] }> {
  const response = await fetch(`${options.baseUrl}/chat/completions`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${options.apiKey}`,
    },
    body: JSON.stringify({
      model: options.model,
      messages: [
        { role: "system", content: SYSTEM_PROMPT },
        { role: "user", content: options.prompt },
      ],
    }),
  });

  if (!response.ok) {
    const errorText = await response.text();
    console.error("Chat completions error:", response.status, errorText);
    throw new ProviderError(
      response.status,
      `LLM provider error (${response.status}). Check that the provider API key is set and the account has credits.`,
    );
  }

  const responseData: unknown = await response.json();
  let content = "";
  if (responseData && typeof responseData === "object" && "choices" in responseData && Array.isArray(responseData.choices)) {
    const first = responseData.choices[0];
    if (first && typeof first === "object" && "message" in first) {
      const message = first.message;
      if (message && typeof message === "object" && "content" in message && typeof message.content === "string") {
        content = message.content;
      }
    }
  }

  return parseMarkdownContent(content);
}
