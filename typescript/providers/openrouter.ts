import { callChatCompletions, TextProvider } from "./contract";

export class OpenRouterProvider implements TextProvider {
  async generateText(prompt: string) {
    return callChatCompletions({
      baseUrl: "https://openrouter.ai/api/v1",
      apiKey: process.env.OPENROUTER_API_KEY,
      model: process.env.OPENROUTER_MODEL || "openai/gpt-oss-20b",
      prompt,
    });
  }
}
