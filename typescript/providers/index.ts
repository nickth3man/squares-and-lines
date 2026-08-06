import { TextProvider } from "./contract";
import { OpenRouterProvider } from "./openrouter";
import { OpenAIProvider } from "./openai";

export type { TextProvider } from "./contract";
export { ProviderError } from "./contract";

const PROVIDERS: Record<string, () => TextProvider> = {
  openrouter: () => new OpenRouterProvider(),
  openai: () => new OpenAIProvider(),
};

/** Builds the configured provider. Selects via AI_PROVIDER (default: openrouter). */
export function createProvider(): TextProvider {
  const name = (process.env.AI_PROVIDER || "openrouter").toLowerCase();
  const create = PROVIDERS[name];
  if (!create) {
    throw new Error(
      `Unknown AI_PROVIDER "${name}". Available providers: ${Object.keys(PROVIDERS).join(", ")}`,
    );
  }
  return create();
}
