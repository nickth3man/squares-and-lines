import { callChatCompletions, TextProvider } from "./contract";

export class OpenAIProvider implements TextProvider {
  async generateText(prompt: string) {
    return callChatCompletions({
      baseUrl: "https://api.openai.com/v1",
      apiKey: process.env.OPENAI_API_KEY,
      model: process.env.OPENAI_MODEL || "gpt-4o-mini",
      prompt,
    });
  }
}
