package main

import (
	"testing"
)

func TestParseMarkdownContent(t *testing.T) {
	tests := []struct {
		name        string
		input       string
		wantText    string
		wantPrompts []string
	}{
		{
			"splits text from explore-further",
			"Some explanatory text about [Topic](Topic) here.\n\n## Explore further\n\n- [First question](First question)\n- [Second question](Second question)\n- [Third question](Third question)",
			"Some explanatory text about [Topic](Topic) here.",
			[]string{"First question", "Second question", "Third question"},
		},
		{
			"text-only when no section",
			"Just some text without a section.",
			"Just some text without a section.",
			[]string{},
		},
		{
			"caps prompts at three",
			"Text here.\n\n## Explore further\n\n- [One](One)\n- [Two](Two)\n- [Three](Three)\n- [Four](Four)\n- [Five](Five)",
			"Text here.",
			[]string{"One", "Two", "Three"},
		},
		{
			"empty string",
			"",
			"",
			[]string{},
		},
		{
			"case-insensitive heading",
			"Text.\n\n## EXPLORE FURTHER\n\n- [Q](Q)",
			"Text.",
			[]string{"Q"},
		},
		{
			"asterisk bullets",
			"Text.\n\n## Explore further\n\n* [Alpha](Alpha)\n* [Beta](Beta)",
			"Text.",
			[]string{"Alpha", "Beta"},
		},
		{
			"no valid bullets in section",
			"Text.\n\n## Explore further\n\nSome prose without bullets.",
			"Text.",
			[]string{},
		},
		{
			"trims whitespace",
			"   padded text   ",
			"padded text",
			[]string{},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := parseMarkdownContent(tt.input)
			if got.Text != tt.wantText {
				t.Errorf("text = %q, want %q", got.Text, tt.wantText)
			}
			if len(got.Prompts) != len(tt.wantPrompts) {
				t.Errorf("prompts len = %d, want %d (%v)", len(got.Prompts), len(tt.wantPrompts), got.Prompts)
				return
			}
			for i, p := range got.Prompts {
				if p != tt.wantPrompts[i] {
					t.Errorf("prompts[%d] = %q, want %q", i, p, tt.wantPrompts[i])
				}
			}
		})
	}
}
