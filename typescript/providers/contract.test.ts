import { describe, it, expect } from 'vitest';
import { parseMarkdownContent } from './contract';

describe('parseMarkdownContent', () => {
  it('extracts inline links and follow-up prompts', () => {
    const raw = `Some explanatory text about [Topic](Topic) and [Another topic](Another topic) here.

## Explore further

- [First question](First question)
- [Second question](Second question)
- [Third question](Third question)`;

    expect(parseMarkdownContent(raw)).toEqual({
      text: 'Some explanatory text about [Topic](Topic) and [Another topic](Another topic) here.',
      links: [
        { label: 'Topic', target: 'Topic' },
        { label: 'Another topic', target: 'Another topic' },
      ],
      prompts: ['First question', 'Second question', 'Third question'],
    });
  });

  it('does not expose explore prompts as inline links', () => {
    expect(parseMarkdownContent('Text with [Term](Term).\n\n## Explore further\n\n- [Prompt](Prompt)').links)
      .toEqual([{ label: 'Term', target: 'Term' }]);
  });

  it('returns text-only when no explore section is present', () => {
    expect(parseMarkdownContent('Just some text without a section.')).toEqual({
      text: 'Just some text without a section.',
      prompts: [],
      links: [],
    });
  });

  it('caps prompts at three even if more are provided', () => {
    const raw = `Text here.

## Explore further

- [One](One)
- [Two](Two)
- [Three](Three)
- [Four](Four)`;
    expect(parseMarkdownContent(raw).prompts).toEqual(['One', 'Two', 'Three']);
  });

  it('handles empty string input', () => {
    expect(parseMarkdownContent('')).toEqual({ text: '', prompts: [], links: [] });
  });
});
