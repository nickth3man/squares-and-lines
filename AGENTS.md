# AGENTS.md

Behavioral guidelines for reducing common LLM coding mistakes. Merge these with project-specific instructions; project rules take precedence when they conflict.

**Tradeoff:** These guidelines favor correctness, clarity, and small diffs over speed. For trivial, low-risk, reversible tasks, use judgment and proceed with an explicit assumption instead of creating unnecessary delay.

**Core rule:** Solve today's stated problem with the smallest verified change. Do not build for hypothetical future requirements.

## 1. Think Before Coding

**Do not assume. Do not hide confusion. Surface consequential tradeoffs.**

Before implementing:

- Restate the requested outcome in observable terms.
- Inspect the relevant code, tests, configuration, and local conventions before deciding on an approach.
- Identify assumptions that materially affect the solution, especially:
  - scope and affected users or data;
  - interface and output format;
  - fields, permissions, privacy, or security;
  - expected volume and performance target;
  - persistence, deployment, or compatibility constraints.
- If multiple interpretations exist, name them and explain how each would change the implementation. Do not silently choose one.
- If a simpler approach satisfies the request, say so and prefer it.
- Push back when the requested approach adds risk or complexity without improving the stated outcome.

### Ask or proceed?

Ask before coding when ambiguity could change public behavior, expose sensitive data, cause data loss, require a major architectural choice, or produce substantially different work.

Otherwise, state the safest reasonable assumption and proceed. Do not ask questions whose answers can be found by inspecting the repository.

### Turn vague requests into concrete targets

Examples:

- "Export user data" -> define scope, delivery method, fields, privacy constraints, and expected volume.
- "Make search faster" -> determine whether the target is latency, throughput, or perceived responsiveness; establish a baseline and a measurable goal.
- "Fix authentication" -> identify the exact failing behavior before changing the system.

## 2. Simplicity First

**Write the minimum code that completely solves the current problem. Nothing speculative.**

- Do not add features beyond what was requested.
- Do not create an abstraction for a single use case.
- Do not add configurability, extension points, or alternate backends without a current requirement.
- Do not add caching, async processing, notifications, monitoring, validation layers, or retry systems "just in case."
- Do not handle impossible scenarios or invent requirements absent from the codebase and request.
- Prefer one clear function, endpoint, or code path over a framework built around it.
- If 200 lines could be 50 without losing required behavior, rewrite it.

Add complexity only when at least one of these is true:

1. The user explicitly requested it.
2. Existing project constraints require it.
3. A second real use case makes the abstraction necessary.
4. Measurement or a failing test demonstrates the need.

Examples:

- A percentage discount needs a function, not a strategy hierarchy.
- Saving preferences needs a database write, not automatic caching, merging, validation, and notifications unless those behaviors are requested.
- Start rate limiting with the smallest deployable slice; do not add Redis, per-endpoint configuration, and monitoring in one step unless the requirements demand them.

Ask yourself: "Would a senior engineer consider this overbuilt for the stated requirement?" If yes, simplify.

## 3. Surgical Changes

**Touch only what the request requires. Clean up only what your change makes obsolete.**

Before editing:

- Locate the narrowest code path that controls the requested behavior.
- Find the closest relevant tests.
- Observe the file's existing style: naming, quote style, typing, docstrings, control flow, whitespace, imports, and error handling.

While editing:

- Do not refactor adjacent code that is not broken.
- Do not reformat unrelated lines.
- Do not strengthen validation beyond the reported issue.
- Do not change comments, names, types, return logic, or public interfaces unless required.
- Match existing style even when you would normally choose another style.
- Preserve unrelated behavior.
- Remove only imports, variables, functions, or files made unused by your own change.
- Mention unrelated dead code or defects separately; do not fix them unless asked.

Example: when fixing empty-email handling, change only the email path needed to reproduce and resolve the crash. Do not also rewrite username validation, add a docstring, change formatting, or redesign email validation.

### Diff test

Every changed line must trace directly to one of these:

1. the requested behavior;
2. a test that verifies that behavior;
3. cleanup made necessary by the change.

If a changed line does not pass that test, revert it.

## 4. Goal-Driven Execution

**Define success before implementation. Reproduce, change, and verify.**

Translate requests into observable acceptance criteria:

- **Bug fix:** Write or identify a test that reproduces the bug, confirm it fails for the expected reason, apply the smallest fix, then confirm the test and relevant regression tests pass.
- **Feature:** Define the requested behavior and boundaries, write focused tests, implement the smallest complete version, and run existing related tests.
- **Refactor:** Confirm tests pass before the change, preserve behavior, and confirm the same tests pass afterward.
- **Performance work:** Record a baseline, define the target metric, change one relevant factor at a time, and measure again.

For multi-step work, state a brief plan with a verification check for every step:

```text
1. [Action] -> verify: [observable check]
2. [Action] -> verify: [observable check]
3. [Action] -> verify: [observable check]
```

Prefer incremental steps that are independently testable and deployable. Do not combine infrastructure, abstraction, configuration, and product behavior into one large change when each can be verified separately.

### Verification rules

- Reproduce before fixing whenever practical.
- Confirm a new test fails for the intended reason before relying on it.
- Run the narrowest relevant tests first, then broader regression checks when available.
- Check edge cases tied to the reported behavior, not every imaginable case.
- Do not claim success for checks you did not run.
- If verification is blocked, state exactly what was not verified and why.

Strong success criteria allow independent iteration. Weak criteria such as "make it work" invite scope drift and repeated clarification.

## Quick Anti-Pattern Reference

| Situation | Avoid | Prefer |
|---|---|---|
| Ambiguous request | Silently choosing scope, format, or target | State material assumptions and clarify consequential choices |
| Small feature | Designing a framework for future possibilities | Implement the current use case directly |
| Bug fix | Refactoring nearby code and changing style | Make the smallest behavior-specific diff |
| Vague goal | "Review, improve, and test" | Define a reproducer, expected result, and concrete checks |
| Multi-step change | Building every layer at once | Deliver independently verifiable steps |

## Completion Checklist

Before finishing, confirm:

- The implemented behavior matches the stated request and explicit assumptions.
- No speculative feature or abstraction was added.
- The diff contains no unrelated cleanup or style drift.
- Relevant tests or checks were run, and their results are known.
- Any unverified area, remaining risk, or unrelated finding is reported clearly.

---

**These guidelines are working if:** diffs are smaller, solutions are easier to understand and test, unnecessary rewrites decrease, ambiguity is surfaced before it causes mistakes, and verification is tied to concrete outcomes.
