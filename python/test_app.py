import unittest

from app import Canvas, GridNodeData, canvas_set_position, parse_markdown_content


class ContractTests(unittest.TestCase):
    def test_extracts_inline_links_and_prompts(self):
        result = parse_markdown_content(
            "Text about [Topic](Topic) and [Another topic](Another topic).\n\n"
            "## Explore further\n\n"
            "- [First question](First question)\n"
            "- [Second question](Second question)\n"
            "- [Third question](Third question)"
        )
        self.assertEqual(result["links"], [
            {"label": "Topic", "target": "Topic"},
            {"label": "Another topic", "target": "Another topic"},
        ])
        self.assertEqual(result["prompts"], ["First question", "Second question", "Third question"])

    def test_explore_prompts_are_not_inline_links(self):
        result = parse_markdown_content("Text with [Term](Term).\n\n## Explore further\n\n- [Prompt](Prompt)")
        self.assertEqual(result["links"], [{"label": "Term", "target": "Term"}])

    def test_position_is_persisted(self):
        node = GridNodeData(id="node-1", x=1, y=2)
        canvas = Canvas(id="canvas-1", nodes=[node])
        canvas_set_position(canvas, "node-1", 30, 40)
        self.assertEqual((node.x, node.y), (30, 40))


if __name__ == "__main__":
    unittest.main()
