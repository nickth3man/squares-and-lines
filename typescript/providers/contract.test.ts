import { describe, it, expect } from 'vitest';
import { parseMarkdownContent } from './contract';

describe('parseMarkdownContent', () => {
  it('splits display text from the explore-further section', () => {
    const raw = `Some explanatory text about [Topic](Topic) here.

## Explore further

- [First question](First question)
- [Second question](Second question)
- [Third question](Third question)`;

    const { text, prompts } = parseMarkdownContent(raw);
    expect(text).toBe('Some explanatory text about [Topic](Topic) here.');
    expect(prompts).toEqual(['First question', 'Second question', 'Third question']);
  });

  it('returns text-only when no explore section is present', () => {
    const { text, prompts } = parseMarkdownContent('Just some text without a section.');
    expect(text).toBe('Just some text without a section.');
    expect(prompts).toEqual([]);
  });

  it('caps prompts at three even if more are provided', () => {
    const raw = `Text here.

## Explore further

- [One](One)
- [Two](Two)
- [Three](Three)
- [Four](Four)
- [Five](Five)`;

    const { prompts } = parseMarkdownContent(raw);
    expect(prompts).toEqual(['One', 'Two', 'Three']);
  });

  it('handles empty string input', () => {
    const { text, prompts } = parseMarkdownContent('');
    expect(text).toBe('');
    expect(prompts).toEqual([]);
  });

  it('matches the heading case-insensitively', () => {
    const raw = `Text.

## EXPLORE FURTHER

- [Q](Q)`;

    const { text, prompts } = parseMarkdownContent(raw);
    expect(text).toBe('Text.');
    expect(prompts).toEqual(['Q']);
  });

  it('accepts asterisk bullets as well as dashes', () => {
    const raw = `Text.

## Explore further

* [Alpha](Alpha)
* [Beta](Beta)`;

    const { prompts } = parseMarkdownContent(raw);
    expect(prompts).toEqual(['Alpha', 'Beta']);
  });

  it('returns empty prompts when the section has no valid bullets', () => {
    const raw = `Text.

## Explore further

Some prose without bullets.`;

    const { text, prompts } = parseMarkdownContent(raw);
    expect(text).toBe('Text.');
    expect(prompts).toEqual([]);
  });

  it('trims surrounding whitespace from the input', () => {
    const { text } = parseMarkdownContent('   padded text   ');
    expect(text).toBe('padded text');
  });
});
