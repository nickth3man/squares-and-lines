package main

import "testing"

func TestParseMarkdownContent(t *testing.T) {
	tests := []struct {
		name        string
		input       string
		wantText    string
		wantPrompts []string
		wantLinks   []inlineLink
	}{
		{
			"inline links and prompts",
			"Text about [Topic](Topic) and [Another topic](Another topic).\n\n## Explore further\n\n- [First question](First question)\n- [Second question](Second question)\n- [Third question](Third question)",
			"Text about [Topic](Topic) and [Another topic](Another topic).",
			[]string{"First question", "Second question", "Third question"},
			[]inlineLink{{Label: "Topic", Target: "Topic"}, {Label: "Another topic", Target: "Another topic"}},
		},
		{
			"links exclude explore section",
			"Text with [Term](Term).\n\n## Explore further\n\n- [Prompt](Prompt)",
			"Text with [Term](Term).",
			[]string{"Prompt"},
			[]inlineLink{{Label: "Term", Target: "Term"}},
		},
		{"text-only when no section", "Just some text.", "Just some text.", []string{}, []inlineLink{}},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := parseMarkdownContent(tt.input)
			if got.Text != tt.wantText {
				t.Fatalf("text = %q, want %q", got.Text, tt.wantText)
			}
			if len(got.Prompts) != len(tt.wantPrompts) || len(got.Links) != len(tt.wantLinks) {
				t.Fatalf("metadata lengths = prompts %d links %d, want prompts %d links %d", len(got.Prompts), len(got.Links), len(tt.wantPrompts), len(tt.wantLinks))
			}
			for i := range tt.wantPrompts {
				if got.Prompts[i] != tt.wantPrompts[i] {
					t.Errorf("prompts[%d] = %q, want %q", i, got.Prompts[i], tt.wantPrompts[i])
				}
			}
			for i := range tt.wantLinks {
				if got.Links[i] != tt.wantLinks[i] {
					t.Errorf("links[%d] = %#v, want %#v", i, got.Links[i], tt.wantLinks[i])
				}
			}
		})
	}
}

func TestCanvasSetPosition(t *testing.T) {
	node := &GridNodeData{ID: "node-1", X: 1, Y: 2}
	canvas := &Canvas{ID: "canvas-1", Nodes: []*GridNodeData{node}}
	if _, err := canvasSetPosition(canvas, "node-1", 30, 40); err != nil {
		t.Fatal(err)
	}
	if node.X != 30 || node.Y != 40 {
		t.Fatalf("position = (%v, %v), want (30, 40)", node.X, node.Y)
	}
}
