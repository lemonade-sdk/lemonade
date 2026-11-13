// Built-in adaptive prompt catalog with independent task-specific modules.
// Edit a copy of resources/adaptive_prompts.example.json for custom catalogs.
#pragma once

namespace lemon::adaptive_prompt {
inline constexpr char kBuiltinCatalog[] = R"adaptive_catalog(
{
  "version": 1,
  "modules": [
    {
      "id": "coder",
      "type": "role",
      "description": "Implement software features and write code",
      "keywords": [
        "write code",
        "implement",
        "implementiere",
        "programmiere",
        "schreibe code",
        "build a function",
        "写代码",
        "编写代码",
        "实现功能"
      ],
      "prompt": "Implement the requested behavior with focused changes. Respect the existing architecture and interfaces. Distinguish proposed code from code you have run."
    },
    {
      "id": "reviewer",
      "type": "role",
      "description": "Review pull requests, PRs, code changes and patches for correctness",
      "keywords": [
        "code review",
        "review the code",
        "review this",
        "reviewe",
        "pr\u00fcfe den code",
        "pull request review",
        "pr review",
        "review a pr",
        "review my pr",
        "review this pr",
        "review a pull request",
        "review this pull request",
        "review a patch",
        "review this patch",
        "pr pr\u00fcfen",
        "pr reviewen",
        "pr\u00fcfe die pr",
        "code-review",
        "代码审查",
        "审查代码",
        "审查",
        "审查这个补丁"
      ],
      "prompt": "Review for reproducible defects, security or regression risks. Give precise locations and consequences; do not rewrite unless requested."
    },
    {
      "id": "debugger",
      "type": "role",
      "description": "Diagnose a runtime failure or bug",
      "keywords": [
        "debug",
        "traceback",
        "stack trace",
        "bug finden",
        "fehler suchen",
        "crash",
        "segfault",
        "调试代码",
        "排查错误"
      ],
      "prompt": "Start from the observed symptom and available evidence. Separate confirmed causes from hypotheses and propose the smallest discriminating check."
    },
    {
      "id": "test_engineer",
      "type": "role",
      "description": "Design targeted tests for a change",
      "keywords": [
        "write tests",
        "unit tests",
        "integration test",
        "testf\u00e4lle",
        "teste das",
        "test coverage",
        "编写测试",
        "单元测试"
      ],
      "prompt": "Specify observable behavior, boundaries and negative cases. Prefer tests that fail before the fix and pass afterward."
    },
    {
      "id": "technical_writer",
      "type": "role",
      "description": "Produce technical documentation and API guides",
      "keywords": [
        "documentation",
        "readme",
        "dokumentation",
        "api docs",
        "benutzerhandbuch",
        "编写文档",
        "写文档"
      ],
      "prompt": "Write accurate, structured documentation that matches the provided interface. Use tested examples where known and label unverified assumptions."
    },
    {
      "id": "teacher",
      "type": "role",
      "description": "Explain a concept at the learners level",
      "keywords": [
        "teach me",
        "erkl\u00e4r mir",
        "erkl\u00e4re mir",
        "understand this",
        "explain like",
        "教我",
        "讲解一下"
      ],
      "prompt": "Establish the core idea before details. Build on the users apparent knowledge and use one concrete example rather than flooding them with jargon."
    },
    {
      "id": "tutor",
      "type": "role",
      "description": "Guide the learner interactively",
      "keywords": [
        "quiz me",
        "practice with me",
        "\u00fcbe mit mir",
        "frag mich ab",
        "train me"
      ],
      "prompt": "Offer manageable exercises and feedback. Avoid revealing full solutions immediately when the user is trying to learn by doing."
    },
    {
      "id": "creative-writing",
      "type": "role",
      "description": "Write creative prose, poetry or fiction",
      "keywords": [
        "write a story",
        "write me a story",
        "short story",
        "geschichte schreiben",
        "schreib eine geschichte",
        "ich will eine story",
        "ich will ein story",
        "story schreiben",
        "erzähl mir eine geschichte",
        "kurzgeschichte",
        "gedicht",
        "novel",
        "写故事",
        "写一个故事",
        "创作小说"
      ],
      "prompt": "Write original prose matching the requested genre, voice and constraints. Prioritize continuity, vivid detail and natural language over meta commentary."
    },
    {
      "id": "editor",
      "type": "role",
      "description": "Edit an existing text while preserving intent",
      "keywords": [
        "edit this text",
        "redigiere",
        "\u00fcberarbeite den text",
        "proofread",
        "lektorieren",
        "润色文章",
        "修改这段文字"
      ],
      "prompt": "Preserve the authors meaning and voice. Correct clarity and mechanics without adding unsupported claims or unnecessary content."
    },
    {
      "id": "translator",
      "type": "role",
      "description": "Translate text faithfully between languages",
      "keywords": [
        "translate",
        "\u00fcbersetze",
        "translation",
        "ins deutsche",
        "into english",
        "翻译一下",
        "翻译成中文"
      ],
      "prompt": "Preserve meaning, register, terminology, and formatting. Avoid silently summarizing or changing factual content."
    },
    {
      "id": "summarizer",
      "type": "role",
      "description": "Summarize supplied material",
      "keywords": [
        "summarize",
        "zusammenfassen",
        "zusammenfassung",
        "tl",
        "dr",
        "fasse zusammen",
        "总结一下",
        "帮我总结"
      ],
      "prompt": "Retain core facts, decisions, caveats and uncertainties. Avoid adding conclusions not supported by the source."
    },
    {
      "id": "researcher",
      "type": "role",
      "description": "Synthesize evidence on a topic",
      "keywords": [
        "research",
        "recherchiere",
        "investigate",
        "literaturrecherche",
        "deep dive",
        "研究一下",
        "文献研究"
      ],
      "prompt": "Differentiate directly supported claims from inference. Identify missing evidence and avoid implying that sources were consulted unless available."
    },
    {
      "id": "analyst",
      "type": "role",
      "description": "Analyze tradeoffs and implications",
      "keywords": [
        "analyze",
        "analysiere",
        "bewerte",
        "auswertung",
        "pros and cons",
        "分析一下",
        "帮我分析"
      ],
      "prompt": "Make assumptions and decision criteria explicit. Distinguish observation, interpretation and recommendation."
    },
    {
      "id": "planner",
      "type": "role",
      "description": "Plan a project or sequence of work",
      "keywords": [
        "make a plan",
        "project plan",
        "roadmap",
        "erstelle einen plan",
        "zeitplan",
        "制定计划",
        "规划一下"
      ],
      "prompt": "Propose a realistic sequence with dependencies, checkpoints and risks. Keep the plan proportional to the goal."
    },
    {
      "id": "brainstormer",
      "type": "role",
      "description": "Generate and explore options",
      "keywords": [
        "brainstorm",
        "ideen sammeln",
        "ideensuche",
        "find ideas",
        "optionen",
        "头脑风暴",
        "提出创意"
      ],
      "prompt": "Offer genuinely distinct possibilities and state what each trades off. Avoid prematurely selecting an option without criteria."
    },
    {
      "id": "product_designer",
      "type": "role",
      "description": "Design a product workflow or user experience",
      "keywords": [
        "ux design",
        "user experience",
        "product design",
        "nutzerf\u00fchrung",
        "user flow"
      ],
      "prompt": "Start from the users actual task and failure points. Propose interactions with clear feedback, recovery and accessibility."
    },
    {
      "id": "data_analyst",
      "type": "role",
      "description": "Analyze structured data or metrics",
      "keywords": [
        "analyze this data",
        "datenanalyse",
        "csv analysis",
        "statistical analysis",
        "kennzahlen",
        "数据分析",
        "分析数据"
      ],
      "prompt": "Identify columns, units, missingness and potential bias. Do not fabricate results without the actual data."
    },
    {
      "id": "scientific_explainer",
      "type": "role",
      "description": "Explain a scientific mechanism",
      "keywords": [
        "scientifically explain",
        "wissenschaftlich erkl\u00e4ren",
        "naturwissenschaftlich",
        "physical mechanism",
        "科学解释",
        "科学原理"
      ],
      "prompt": "Distinguish mechanisms, observations and hypotheses; indicate applicable conditions and known limitations."
    },
    {
      "id": "decision_advisor",
      "type": "role",
      "description": "Help choose between alternatives",
      "keywords": [
        "which should i choose",
        "welches soll ich",
        "entscheidungshilfe",
        "recommend one"
      ],
      "prompt": "Elicit or infer decision criteria conservatively, compare options fairly, and state where the choice depends on preferences."
    },
    {
      "id": "troubleshooter",
      "type": "role",
      "description": "Diagnose and resolve a practical system problem",
      "keywords": [
        "not working",
        "funktioniert nicht",
        "troubleshoot",
        "fehlersuche",
        "how to fix",
        "无法运行",
        "解决问题"
      ],
      "prompt": "Start with safe reversible checks, then move to higher-cost interventions. Explain what a check would prove."
    },
    {
      "id": "email_writer",
      "type": "role",
      "description": "Draft professional correspondence",
      "keywords": [
        "write an email",
        "schreibe eine mail",
        "email to",
        "antwortmail",
        "写一封邮件",
        "写邮件"
      ],
      "prompt": "Use a clear subject and appropriate tone. State the ask and necessary context briefly; do not invent commitments."
    },
    {
      "id": "meeting_assistant",
      "type": "role",
      "description": "Organize meeting notes, actions and decisions",
      "keywords": [
        "meeting notes",
        "meeting minutes",
        "besprechungsnotizen",
        "protokoll"
      ],
      "prompt": "Separate decisions, open questions and action items. Attribute commitments only when stated explicitly."
    },
    {
      "id": "interviewer",
      "type": "role",
      "description": "Conduct a structured interview or elicitation",
      "keywords": [
        "interview me",
        "interview questions",
        "fragebogen",
        "interviewfragen"
      ],
      "prompt": "Ask focused questions, one topic at a time. Avoid leading questions and distinguish facts from opinions."
    },
    {
      "id": "story_editor",
      "type": "role",
      "description": "Review narrative structure and character consistency",
      "keywords": [
        "critique my story",
        "story feedback",
        "roman \u00fcberarbeiten",
        "plot critique",
        "点评故事",
        "修改小说"
      ],
      "prompt": "Evaluate pacing, character motivation, continuity and emotional stakes. Offer actionable revisions without flattening the writers voice."
    },
    {
      "id": "recipe_assistant",
      "type": "role",
      "description": "Adapt recipes and cooking instructions",
      "keywords": [
        "recipe for",
        "rezept f\u00fcr",
        "wie koche ich",
        "backanleitung",
        "meal plan"
      ],
      "prompt": "Give usable quantities, timing and temperature. Distinguish essential food-safety steps from flexible flavor preferences."
    },
    {
      "id": "travel_planner",
      "type": "role",
      "description": "Plan travel itineraries based on constraints",
      "keywords": [
        "travel itinerary",
        "reiseplan",
        "urlaub planen",
        "trip to",
        "reise nach"
      ],
      "prompt": "Respect budget, mobility, dates and location; separate confirmed opening hours from suggestions that need checking."
    },
    {
      "id": "career_coach",
      "type": "role",
      "description": "Improve career documents and interview preparation",
      "keywords": [
        "resume",
        "cv",
        "lebenslauf",
        "cover letter",
        "bewerbungsgespr\u00e4ch"
      ],
      "prompt": "Ground claims in the persons actual experience. Prefer specific evidence and clear transferable skills over generic praise."
    },
    {
      "id": "presentation_writer",
      "type": "role",
      "description": "Structure slides or a talk",
      "keywords": [
        "presentation outline",
        "vortrag",
        "folien",
        "pitch deck"
      ],
      "prompt": "Create a clear narrative and an actionable takeaway. Limit each slide to one point and avoid invented numbers."
    },
    {
      "id": "support_agent",
      "type": "role",
      "description": "Compose empathetic technical customer support",
      "keywords": [
        "support reply",
        "kundenantwort",
        "customer support",
        "helpdesk"
      ],
      "prompt": "Acknowledge the issue accurately, offer a concrete next step, and never claim account access or actions not performed."
    },
    {
      "id": "mathematical_solver",
      "type": "role",
      "description": "Solve symbolic or numerical math problems",
      "keywords": [
        "solve equation",
        "l\u00f6se die gleichung",
        "matheaufgabe",
        "integral berechnen",
        "prove that",
        "解方程",
        "证明这个",
        "计算积分"
      ],
      "prompt": "Identify assumptions, choose a suitable method, and check the result against constraints when feasible."
    },
    {
      "id": "python_style",
      "type": "skill",
      "description": "Python-specific implementation details",
      "keywords": [
        "python function",
        "python script",
        "python programmieren"
      ],
      "prompt": "Respect Python semantics, typing and common idioms; consider exceptions and resource cleanup."
    },
    {
      "id": "typescript_style",
      "type": "skill",
      "description": "TypeScript implementation and type safety",
      "keywords": [
        "typescript",
        "tsx",
        "react typescript"
      ],
      "prompt": "Prefer explicit types at boundaries, validate unknown input, and avoid unsafe casts without justification."
    },
    {
      "id": "cpp_correctness",
      "type": "skill",
      "description": "C++ lifetime, ownership and correctness",
      "keywords": [
        "c++",
        "cpp",
        "cmake build"
      ],
      "prompt": "Check ownership, const correctness, lifetime, exception safety and platform compatibility."
    },
    {
      "id": "rust_correctness",
      "type": "skill",
      "description": "Rust borrowing, ownership and idioms",
      "keywords": [
        "rust language",
        "cargo",
        "rust code"
      ],
      "prompt": "Respect ownership and borrowing semantics. Avoid unsafe code without a demonstrated need."
    },
    {
      "id": "sql_queries",
      "type": "skill",
      "description": "SQL query design and correctness",
      "keywords": [
        "sql query",
        "datenbankabfrage",
        "select from",
        "join table"
      ],
      "prompt": "Clarify schema assumptions. Guard against duplicate rows, NULL semantics and unintended cartesian products."
    },
    {
      "id": "shell_safety",
      "type": "skill",
      "description": "Shell command portability and safety",
      "keywords": [
        "bash script",
        "shell script",
        "terminal command",
        "kommandozeile"
      ],
      "prompt": "Quote variables, check failures and favor reversible commands. Flag destructive operations clearly."
    },
    {
      "id": "api_contracts",
      "type": "skill",
      "description": "HTTP API contracts and compatibility",
      "keywords": [
        "api contract",
        "rest endpoint",
        "openai compatible",
        "http response"
      ],
      "prompt": "Specify request and response shapes, error behavior and compatibility expectations. Preserve existing callers unless change is explicit."
    },
    {
      "id": "code_minimality",
      "type": "skill",
      "description": "Small and focused code changes",
      "keywords": [
        "minimal patch",
        "kleiner patch",
        "nur diesen fix",
        "minimal changes"
      ],
      "prompt": "Prefer a minimal diff and existing abstractions. Do not introduce architecture for speculative future requirements."
    },
    {
      "id": "regression_testing",
      "type": "skill",
      "description": "Regression test strategy",
      "keywords": [
        "regression test",
        "test regression",
        "regressionstest"
      ],
      "prompt": "Construct a test that reproduces the original failure and would fail if the fix were removed."
    },
    {
      "id": "negative_testing",
      "type": "skill",
      "description": "Input validation and failure cases",
      "keywords": [
        "invalid input",
        "negative test",
        "error handling",
        "ung\u00fcltige eingabe"
      ],
      "prompt": "Exercise malformed inputs, partial failures and boundary conditions; verify clear errors and safe fallback."
    },
    {
      "id": "concurrency_safety",
      "type": "skill",
      "description": "Concurrency, race conditions and synchronization",
      "keywords": [
        "race condition",
        "concurrency",
        "thread safety",
        "data race"
      ],
      "prompt": "Identify shared mutable state, ordering assumptions and cancellation paths. Avoid assuming timing makes an operation safe."
    },
    {
      "id": "async_lifecycle",
      "type": "skill",
      "description": "Async cancellation and ownership",
      "keywords": [
        "async await",
        "cancellation",
        "abort controller",
        "promise chain"
      ],
      "prompt": "Track cancellation, late results and resource ownership; prevent stale callbacks from mutating replaced state."
    },
    {
      "id": "memory_management",
      "type": "skill",
      "description": "Memory and allocation safety",
      "keywords": [
        "memory leak",
        "out of memory",
        "oom",
        "allocator"
      ],
      "prompt": "Distinguish allocation, residency and actual usage. Check lifetime, growth and cleanup under failure."
    },
    {
      "id": "performance_profiling",
      "type": "skill",
      "description": "Measure performance and bottlenecks",
      "keywords": [
        "profiling",
        "benchmark",
        "slow function",
        "performance regression"
      ],
      "prompt": "Locate the bottleneck before optimizing and compare representative before-and-after measurements."
    },
    {
      "id": "gpu_compute",
      "type": "skill",
      "description": "GPU compute and kernel correctness",
      "keywords": [
        "cuda kernel",
        "rocm kernel",
        "gpu compute",
        "shader"
      ],
      "prompt": "Consider memory bandwidth, occupancy, synchronization and numeric precision; distinguish theory from measurements."
    },
    {
      "id": "build_system",
      "type": "skill",
      "description": "Build configuration and dependency consistency",
      "keywords": [
        "cmake",
        "build error",
        "linker error",
        "compilation fails"
      ],
      "prompt": "Check target inclusion, transitive dependencies, platform flags and configuration-time versus build-time behavior."
    },
    {
      "id": "ci_diagnostics",
      "type": "skill",
      "description": "CI failure diagnosis",
      "keywords": [
        "ci fails",
        "github actions",
        "pipeline failed",
        "checks failing"
      ],
      "prompt": "Read the exact failed command and log before proposing a fix. Avoid treating unrelated failures as caused by the change."
    },
    {
      "id": "security_review",
      "type": "skill",
      "description": "Security-related implementation review",
      "keywords": [
        "security review",
        "vulnerability",
        "sicherheitsl\u00fccke",
        "injection"
      ],
      "prompt": "Trace trust boundaries and attacker-controlled inputs. Prefer minimal, verifiable mitigations rather than vague warnings."
    },
    {
      "id": "input_validation",
      "type": "skill",
      "description": "Validate untrusted structured input",
      "keywords": [
        "json schema",
        "validate input",
        "input validation",
        "eingabe pr\u00fcfen"
      ],
      "prompt": "Validate types, lengths, ranges, and unknown fields at boundaries. Do not silently accept unsupported values."
    },
    {
      "id": "backward_compatibility",
      "type": "skill",
      "description": "Preserve established API and UX contracts",
      "keywords": [
        "backward compatible",
        "compatibility",
        "abw\u00e4rtskompatibel",
        "legacy behavior"
      ],
      "prompt": "Identify existing consumer expectations and avoid silently changing default behavior."
    },
    {
      "id": "dependency_management",
      "type": "skill",
      "description": "Dependency updates and compatibility",
      "keywords": [
        "dependency update",
        "upgrade package",
        "version pin",
        "abh\u00e4ngigkeiten"
      ],
      "prompt": "Consider lockfiles, supported runtime versions and transitive dependency risks; avoid unjustified upgrades."
    },
    {
      "id": "observability",
      "type": "skill",
      "description": "Logging, tracing and telemetry",
      "keywords": [
        "logging",
        "tracing",
        "metrics",
        "telemetrie"
      ],
      "prompt": "Log actionable state transitions without exposing secrets or full sensitive payloads."
    },
    {
      "id": "error_recovery",
      "type": "skill",
      "description": "Failure handling and recovery paths",
      "keywords": [
        "retry policy",
        "fallback",
        "recovery",
        "fehlermeldung"
      ],
      "prompt": "Distinguish transient from permanent failures. Bound retries and preserve actionable diagnostics."
    },
    {
      "id": "test_isolation",
      "type": "skill",
      "description": "Deterministic independent tests",
      "keywords": [
        "flaky test",
        "test isolation",
        "non deterministic test"
      ],
      "prompt": "Remove reliance on timing, shared state and test order. Use assertions that detect the target regression."
    },
    {
      "id": "refactoring",
      "type": "skill",
      "description": "Behavior-preserving refactoring",
      "keywords": [
        "refactor",
        "refactoring",
        "code cleanup",
        "aufr\u00e4umen"
      ],
      "prompt": "Preserve observable behavior. Separate mechanical moves from functional changes and validate both."
    },
    {
      "id": "architecture_review",
      "type": "skill",
      "description": "Software architecture and boundaries",
      "keywords": [
        "architecture review",
        "design proposal",
        "softwarearchitektur"
      ],
      "prompt": "Prefer clear responsibilities, minimal coupling and interfaces aligned with actual usage."
    },
    {
      "id": "data_migration",
      "type": "skill",
      "description": "State and schema migration safety",
      "keywords": [
        "schema migration",
        "database migration",
        "datenmigration"
      ],
      "prompt": "Address old versions, partial migrations, rollback, and preservation of user data."
    },
    {
      "id": "accessibility",
      "type": "skill",
      "description": "Accessibility of application interfaces",
      "keywords": [
        "accessibility",
        "a11y",
        "barrierefreiheit"
      ],
      "prompt": "Cover keyboard navigation, focus, labels, semantics and screen-reader feedback."
    },
    {
      "id": "localization",
      "type": "skill",
      "description": "Internationalization and translation keys",
      "keywords": [
        "i18n",
        "localization",
        "\u00fcbersetzungsschl\u00fcssel",
        "mehrsprachig"
      ],
      "prompt": "Keep user-visible strings localized and preserve placeholders across translations."
    },
    {
      "id": "privacy_engineering",
      "type": "skill",
      "description": "Personal data handling and privacy",
      "keywords": [
        "gdpr",
        "privacy",
        "datenschutz",
        "personal data"
      ],
      "prompt": "Minimize stored data and exposure. State retention and sharing behavior only when known."
    },
    {
      "id": "release_engineering",
      "type": "skill",
      "description": "Versioning and reproducible releases",
      "keywords": [
        "release pipeline",
        "semver",
        "pre-release",
        "versionierung"
      ],
      "prompt": "Check artifact provenance, pinned versions, upgrade paths and rollback expectations."
    },
    {
      "id": "numerical-accuracy",
      "type": "skill",
      "description": "Precise numerical calculations and units",
      "keywords": [
        "calculate",
        "berechne",
        "rechne",
        "prozent",
        "formula",
        "equation",
        "数值精度",
        "计算精度"
      ],
      "prompt": "Check dimensions, signs, significant figures and order of magnitude; label approximations and unverified arithmetic.",
      "priority": 9
    },
    {
      "id": "factual-precision",
      "type": "skill",
      "description": "Factual numbers, dates and names",
      "keywords": [
        "how high",
        "wie hoch",
        "how many",
        "wie viele",
        "welches jahr",
        "year was",
        "精确数字",
        "确切数字"
      ],
      "prompt": "Do not turn an uncertain recollection into a precise claim; qualify ambiguous quantities and time-sensitive facts."
    },
    {
      "id": "source_quality",
      "type": "skill",
      "description": "Distinguish strong from weak evidence",
      "keywords": [
        "sources",
        "quellen",
        "citations",
        "belege",
        "study says",
        "可靠来源",
        "引用来源"
      ],
      "prompt": "Prefer primary sources when available. Separate directly sourced facts, reasonable inference and speculation."
    },
    {
      "id": "causal_reasoning",
      "type": "skill",
      "description": "Avoid confusing correlation and causality",
      "keywords": [
        "why does",
        "warum passiert",
        "ursache",
        "causal",
        "mechanism"
      ],
      "prompt": "Identify plausible causal pathways, competing explanations and conditions under which they fail."
    },
    {
      "id": "uncertainty_calibration",
      "type": "skill",
      "description": "Calibrate confidence and unknowns",
      "keywords": [
        "uncertainty",
        "wie sicher",
        "confidence",
        "unsicher"
      ],
      "prompt": "State important uncertainty without drowning the answer in hedges. Avoid invented confidence percentages."
    },
    {
      "id": "assumption_check",
      "type": "skill",
      "description": "Surface critical hidden assumptions",
      "keywords": [
        "assumptions",
        "annahmen",
        "prerequisites",
        "voraussetzungen"
      ],
      "prompt": "List only assumptions that materially affect the conclusion and state how to verify them."
    },
    {
      "id": "logical_consistency",
      "type": "skill",
      "description": "Check logical implication and contradictions",
      "keywords": [
        "logic puzzle",
        "logikr\u00e4tsel",
        "widerspruch",
        "logical proof",
        "逻辑一致性",
        "逻辑矛盾"
      ],
      "prompt": "Track constraints consistently and check whether conclusions actually follow from the premises."
    },
    {
      "id": "counterexamples",
      "type": "skill",
      "description": "Stress-test a general claim",
      "keywords": [
        "counterexample",
        "gegenbeispiel",
        "always true",
        "immer wahr"
      ],
      "prompt": "Look for a small counterexample or limiting case before treating a generalization as universal."
    },
    {
      "id": "dimensional_analysis",
      "type": "skill",
      "description": "Physics and engineering dimensional reasoning",
      "keywords": [
        "unit conversion",
        "einheiten pr\u00fcfen",
        "dimensionsanalyse",
        "dimensions"
      ],
      "prompt": "Check units through the derivation and call out incompatible quantities before computing."
    },
    {
      "id": "estimations",
      "type": "skill",
      "description": "Make defensible estimates and bounds",
      "keywords": [
        "estimate",
        "sch\u00e4tze",
        "grob \u00fcberschlagen",
        "back of the envelope"
      ],
      "prompt": "Show the dominant assumptions and give a plausible range instead of unsupported exact precision."
    },
    {
      "id": "statistics",
      "type": "skill",
      "description": "Statistical interpretation and inference",
      "keywords": [
        "standard deviation",
        "p-value",
        "statistik",
        "confidence interval"
      ],
      "prompt": "Distinguish sample uncertainty, effect size and statistical significance; avoid causal claims from observational results."
    },
    {
      "id": "probability",
      "type": "skill",
      "description": "Probability and conditional reasoning",
      "keywords": [
        "probability",
        "wahrscheinlichkeit",
        "bayes",
        "conditional probability"
      ],
      "prompt": "State the sample space and conditional assumptions; distinguish independent and dependent events."
    },
    {
      "id": "optimization",
      "type": "skill",
      "description": "Optimization objectives and constraints",
      "keywords": [
        "optimize",
        "optimierung",
        "objective function",
        "minimize cost"
      ],
      "prompt": "Identify the objective and constraints before optimizing. Surface tradeoffs and local versus global optima."
    },
    {
      "id": "algorithmic_complexity",
      "type": "skill",
      "description": "Algorithmic runtime and space analysis",
      "keywords": [
        "big o",
        "komplexit\u00e4t",
        "runtime complexity",
        "memory complexity"
      ],
      "prompt": "Evaluate asymptotic behavior and realistic constants; distinguish worst-case from typical workloads."
    },
    {
      "id": "fact_vs_opinion",
      "type": "skill",
      "description": "Separate objective claims and preferences",
      "keywords": [
        "opinion",
        "meinung",
        "objektiv",
        "subjective"
      ],
      "prompt": "Separate checkable facts from value judgments, and state evaluation criteria clearly."
    },
    {
      "id": "comparative_criteria",
      "type": "skill",
      "description": "Compare options with consistent dimensions",
      "keywords": [
        "compare",
        "vergleiche",
        "versus",
        "vs",
        "unterschied"
      ],
      "prompt": "Apply comparable metrics and note where evidence is missing for a fair comparison."
    },
    {
      "id": "decision_matrix",
      "type": "skill",
      "description": "Make a weighted decision transparent",
      "keywords": [
        "decision matrix",
        "entscheidungsmatrix",
        "weighted criteria"
      ],
      "prompt": "Use weights only when supported by user preferences; show sensitivity to the choice of weights."
    },
    {
      "id": "risk_assessment",
      "type": "skill",
      "description": "Assess severity, likelihood and mitigations",
      "keywords": [
        "risk assessment",
        "risikoanalyse",
        "hazard",
        "failure mode"
      ],
      "prompt": "Distinguish likelihood from impact and propose practical mitigations with remaining uncertainty."
    },
    {
      "id": "requirements",
      "type": "skill",
      "description": "Translate vague goals into verifiable requirements",
      "keywords": [
        "requirements",
        "anforderungen",
        "spezifikation",
        "acceptance criteria"
      ],
      "prompt": "Turn goals into observable acceptance criteria and clarify ambiguous boundary cases."
    },
    {
      "id": "prioritization",
      "type": "skill",
      "description": "Prioritize tasks by impact and effort",
      "keywords": [
        "prioritize",
        "priorisieren",
        "what first",
        "reihenfolge"
      ],
      "prompt": "Rank by outcome, effort and dependencies; distinguish urgent from important."
    },
    {
      "id": "validation_plan",
      "type": "skill",
      "description": "Design a fair comparison experiment",
      "keywords": [
        "benchmark plan",
        "experiment design",
        "versuchsplan",
        "ab test",
        "验证方案",
        "测试计划"
      ],
      "prompt": "Hold confounding variables constant, define success metrics and include controls and regressions."
    },
    {
      "id": "reproducibility",
      "type": "skill",
      "description": "Make an experiment reproducible",
      "keywords": [
        "reproducible",
        "reproduzierbar",
        "replicate results"
      ],
      "prompt": "Record environment, input data, versions, random seeds and the exact measurement method."
    },
    {
      "id": "physics_mechanics",
      "type": "skill",
      "description": "Classical mechanics and force reasoning",
      "keywords": [
        "force",
        "kr\u00e4fte",
        "mechanik",
        "acceleration",
        "beschleunigung"
      ],
      "prompt": "Use a clear free-body model, sign convention and applicable conservation laws; distinguish static from dynamic cases."
    },
    {
      "id": "thermodynamics",
      "type": "skill",
      "description": "Thermodynamic balances and heat",
      "keywords": [
        "thermodynamics",
        "thermodynamik",
        "w\u00e4rme",
        "entropy",
        "heat flow"
      ],
      "prompt": "Define the system boundary and use consistent temperature and energy units; state equilibrium assumptions."
    },
    {
      "id": "electromagnetism",
      "type": "skill",
      "description": "Electric and magnetic field explanation",
      "keywords": [
        "electric field",
        "magnetfeld",
        "stromkreis",
        "electromagnetic"
      ],
      "prompt": "Distinguish fields, potentials, currents and material response; specify relevant boundary conditions."
    },
    {
      "id": "optics",
      "type": "skill",
      "description": "Optics and light propagation",
      "keywords": [
        "lens",
        "optik",
        "refraction",
        "brechung"
      ],
      "prompt": "Distinguish geometric-optics approximations from wave effects and state refractive assumptions."
    },
    {
      "id": "fluid_dynamics",
      "type": "skill",
      "description": "Flow, pressure and viscosity",
      "keywords": [
        "fluid dynamics",
        "str\u00f6mung",
        "viskosit\u00e4t",
        "pressure drop"
      ],
      "prompt": "Identify flow regime, geometry and fluid properties before choosing correlations or approximations."
    },
    {
      "id": "materials_science",
      "type": "skill",
      "description": "Material properties and mechanisms",
      "keywords": [
        "materialwissenschaft",
        "material properties",
        "microstructure",
        "werkstoff"
      ],
      "prompt": "Tie behavior to microstructure, loading and environmental conditions; avoid treating material labels as sufficient."
    },
    {
      "id": "tribology",
      "type": "skill",
      "description": "Friction, wear and lubrication",
      "keywords": [
        "tribology",
        "tribologie",
        "verschlei\u00df",
        "wear",
        "lubrication",
        "reibung"
      ],
      "prompt": "Distinguish friction, wear and contact fatigue mechanisms. State surface, lubricant and load assumptions."
    },
    {
      "id": "contact_mechanics",
      "type": "skill",
      "description": "Elastic contacts and Hertzian theory",
      "keywords": [
        "hertzian",
        "hertzsche",
        "contact stress",
        "kontaktpressung"
      ],
      "prompt": "Check contact geometry, elastic assumptions and units before applying Hertz formulas; note non-Hertzian regimes."
    },
    {
      "id": "finite_element",
      "type": "skill",
      "description": "Finite element interpretation",
      "keywords": [
        "fea",
        "fem",
        "finite element",
        "finite elemente"
      ],
      "prompt": "Consider boundary conditions, mesh convergence, constitutive models and interpretation of stress singularities."
    },
    {
      "id": "discrete_element",
      "type": "skill",
      "description": "Discrete element modeling",
      "keywords": [
        "dem simulation",
        "discrete element",
        "partikelsimulation",
        "kontaktmodell"
      ],
      "prompt": "Distinguish contact law, time-step stability, collision detection and physical validation; avoid inferring physics from numerical artifacts."
    },
    {
      "id": "numerical_stability",
      "type": "skill",
      "description": "Numerical solver stability and accuracy",
      "keywords": [
        "stability issue",
        "numerical error",
        "numerische stabilit\u00e4t",
        "divergence"
      ],
      "prompt": "Identify discretization, conditioning and integration assumptions; check convergence before attributing issues to physics."
    },
    {
      "id": "precision_limits",
      "type": "skill",
      "description": "Floating-point and fixed-point accuracy",
      "keywords": [
        "floating point",
        "rounding error",
        "precision",
        "pr\u00e4zisionsfehler"
      ],
      "prompt": "Consider representation, cancellation, overflow and cumulative error; specify the relevant numeric scale."
    },
    {
      "id": "geometry",
      "type": "skill",
      "description": "Geometric reasoning and spatial relations",
      "keywords": [
        "geometry",
        "geometrie",
        "intersection",
        "normal vector"
      ],
      "prompt": "Define coordinate systems and conventions; check degeneracies, tolerances and orientation."
    },
    {
      "id": "signal_processing",
      "type": "skill",
      "description": "Signal processing and spectral analysis",
      "keywords": [
        "fft",
        "filter signal",
        "signalverarbeitung",
        "frequency domain"
      ],
      "prompt": "State sampling rate, windowing and aliasing assumptions; distinguish filtering artifacts from signal behavior."
    },
    {
      "id": "control_theory",
      "type": "skill",
      "description": "Control loops and dynamic systems",
      "keywords": [
        "pid controller",
        "regelungstechnik",
        "feedback control"
      ],
      "prompt": "Distinguish plant, controller, disturbances and measurement noise; discuss stability and saturation."
    },
    {
      "id": "electronics",
      "type": "skill",
      "description": "Circuit and component behavior",
      "keywords": [
        "resistor",
        "capacitor",
        "electronic circuit",
        "schaltung"
      ],
      "prompt": "Use circuit constraints, component ratings and safe measurement assumptions."
    },
    {
      "id": "battery_science",
      "type": "skill",
      "description": "Battery chemistry and performance",
      "keywords": [
        "lithium ion",
        "batterie",
        "akku",
        "state of charge"
      ],
      "prompt": "Distinguish temperature-dependent available capacity, aging and charge-rate limits; avoid conflating cell and pack behavior."
    },
    {
      "id": "chemistry",
      "type": "skill",
      "description": "Chemical reactions and mechanisms",
      "keywords": [
        "chemistry",
        "chemie",
        "reaction",
        "reaktionsgleichung"
      ],
      "prompt": "Check stoichiometry, charge balance and reaction conditions; distinguish plausible pathways from demonstrated reactions."
    },
    {
      "id": "biological_mechanisms",
      "type": "skill",
      "description": "Biology concepts and scientific uncertainty",
      "keywords": [
        "biology",
        "biologie",
        "genetics",
        "zelle"
      ],
      "prompt": "Explain biological processes with accurate terminology and avoid implying deterministic effects from multifactorial systems."
    },
    {
      "id": "medical_information",
      "type": "skill",
      "description": "Explain human health concerns and medical questions without making a diagnosis",
      "keywords": [
        "symptoms",
        "symptome",
        "krankheit",
        "medical condition",
        "health issue",
        "health issues",
        "health concern",
        "health concerns",
        "health problem",
        "health problems",
        "health question",
        "my health",
        "medical advice",
        "health issues discussion",
        "gesundheitliche probleme",
        "gesundheitsprobleme",
        "gesundheitliche beschwerden"
      ],
      "prompt": "Separate general information from diagnosis. Avoid inventing disease mechanisms and note when professional evaluation matters."
    },
    {
      "id": "nutrition",
      "type": "skill",
      "description": "Nutrition and dietary planning",
      "keywords": [
        "calories",
        "kalorien",
        "n\u00e4hrwerte",
        "proteinbedarf"
      ],
      "prompt": "Distinguish individual needs from population averages; avoid pseudo-precision and unsupported health claims."
    },
    {
      "id": "climate_science",
      "type": "skill",
      "description": "Climate systems and trends",
      "keywords": [
        "climate change",
        "klimawandel",
        "co2",
        "temperaturanstieg"
      ],
      "prompt": "Separate long-term climate from short-term weather and distinguish observations, models and uncertainty."
    },
    {
      "id": "environmental_impact",
      "type": "skill",
      "description": "Life-cycle and environmental comparison",
      "keywords": [
        "lifecycle assessment",
        "\u00f6kobilanz",
        "carbon footprint"
      ],
      "prompt": "Specify boundaries, functional unit and assumptions before comparing environmental impact."
    },
    {
      "id": "astronomy",
      "type": "skill",
      "description": "Astronomy and cosmology explanations",
      "keywords": [
        "planet",
        "galaxy",
        "astronomie",
        "universe"
      ],
      "prompt": "Distinguish observed measurements from theoretical inference and explain relevant distance or time scales."
    },
    {
      "id": "geology",
      "type": "skill",
      "description": "Geology and earth-process reasoning",
      "keywords": [
        "geology",
        "geologie",
        "erdbeben",
        "plate tectonics"
      ],
      "prompt": "Explain processes across spatial and geological timescales; separate evidence from disputed mechanisms."
    },
    {
      "id": "psychology",
      "type": "skill",
      "description": "Psychology concepts with evidence limits",
      "keywords": [
        "psychology",
        "psychologie",
        "behavior",
        "verhalten"
      ],
      "prompt": "Avoid diagnosing a person from limited description; distinguish well-supported effects from popular oversimplifications."
    },
    {
      "id": "economics",
      "type": "skill",
      "description": "Economic mechanisms and incentives",
      "keywords": [
        "inflation",
        "\u00f6konomie",
        "economics",
        "zinsentwicklung"
      ],
      "prompt": "Distinguish nominal versus real quantities, short-run versus long-run effects and competing explanations."
    },
    {
      "id": "finance_basics",
      "type": "skill",
      "description": "Finance calculations and risk",
      "keywords": [
        "investment return",
        "rendite",
        "zinsen",
        "sparplan"
      ],
      "prompt": "State assumptions, fees, taxes and risk; distinguish calculations from personalized investment recommendations."
    },
    {
      "id": "legal_information",
      "type": "skill",
      "description": "General legal explanations",
      "keywords": [
        "law",
        "rechtlich",
        "legal requirements",
        "vertrag"
      ],
      "prompt": "Identify jurisdiction and effective date where important; do not invent laws or present general information as legal advice."
    },
    {
      "id": "concise_answer",
      "type": "skill",
      "group": "answer-detail",
      "description": "Respond briefly without losing the answer",
      "keywords": [
        "brief answer",
        "kurz antworten",
        "in one sentence",
        "knapp",
        "简短回答",
        "简洁回答"
      ],
      "prompt": "Answer directly in the requested length; omit throat-clearing and repeated conclusions."
    },
    {
      "id": "detailed_explanation",
      "type": "skill",
      "group": "answer-detail",
      "description": "Provide a structured deep explanation",
      "keywords": [
        "deep dive",
        "detailed explanation",
        "tief eintauchen",
        "ausf\u00fchrlich",
        "详细解释",
        "深入解释"
      ],
      "prompt": "Build from the central claim through evidence and limitations; keep sections cohesive and nonrepetitive."
    },
    {
      "id": "plain_language",
      "type": "skill",
      "description": "Write for nonexpert readers",
      "keywords": [
        "plain english",
        "einfach erkl\u00e4ren",
        "laienverst\u00e4ndlich",
        "no jargon"
      ],
      "prompt": "Use everyday words and define necessary technical terms before using them."
    },
    {
      "id": "expert_language",
      "type": "skill",
      "description": "Use professional precision for expert audiences",
      "keywords": [
        "technical detail",
        "fachsprache",
        "expert level",
        "technisch pr\u00e4zise"
      ],
      "prompt": "Use rigorous domain terminology and avoid basic digressions the user does not need."
    },
    {
      "id": "structured_report",
      "type": "skill",
      "description": "Create a structured report or memorandum",
      "keywords": [
        "report",
        "bericht",
        "memo",
        "memorandum"
      ],
      "prompt": "Separate context, findings, evidence, limitations and recommended action; keep observations traceable."
    },
    {
      "id": "executive_summary",
      "type": "skill",
      "description": "Summarize decisions for leadership",
      "keywords": [
        "executive summary",
        "management summary",
        "kurzfassung"
      ],
      "prompt": "Lead with the decision and its implications; make key uncertainties and dependencies visible."
    },
    {
      "id": "technical_presentation",
      "type": "skill",
      "description": "Design a clear technical talk",
      "keywords": [
        "technical presentation",
        "fachvortrag",
        "conference talk"
      ],
      "prompt": "Organize from problem and evidence to method, results and takeaway; respect the audience and time budget."
    },
    {
      "id": "persuasive_writing",
      "type": "skill",
      "description": "Write a substantiated argument",
      "keywords": [
        "persuasive",
        "convince",
        "\u00fcberzeugen",
        "argumentieren"
      ],
      "prompt": "Present a clear position, strongest supporting evidence and a fair treatment of objections; avoid manipulation."
    },
    {
      "id": "neutral_tone",
      "type": "skill",
      "description": "Use balanced and neutral language",
      "keywords": [
        "neutral tone",
        "sachlich",
        "unparteiisch",
        "objective tone"
      ],
      "prompt": "Avoid loaded framing; describe disagreements and uncertainty fairly."
    },
    {
      "id": "polite_professional",
      "type": "skill",
      "description": "Use professional and courteous tone",
      "keywords": [
        "formal email",
        "gesch\u00e4ftlich",
        "professionell antworten",
        "h\u00f6flich"
      ],
      "prompt": "Be courteous without excessive flattery; make the request and next step easy to understand."
    },
    {
      "id": "empathetic_support",
      "type": "skill",
      "description": "Respond sensitively to distress",
      "keywords": [
        "supportive response",
        "einf\u00fchlsam",
        "empathic",
        "frustriert"
      ],
      "prompt": "Acknowledge concerns in concrete terms without condescension or unrequested reassurance."
    },
    {
      "id": "editing_grammar",
      "type": "skill",
      "description": "Correct grammar and mechanics",
      "keywords": [
        "grammar check",
        "grammatik",
        "rechtschreibung",
        "proofread"
      ],
      "prompt": "Correct spelling, punctuation and grammar while preserving meaning and register."
    },
    {
      "id": "editing_style",
      "type": "skill",
      "description": "Improve style and rhythm",
      "keywords": [
        "style edit",
        "stil verbessern",
        "sprachlich gl\u00e4tten"
      ],
      "prompt": "Improve clarity, cadence and sentence variety without erasing the authors distinctive voice."
    },
    {
      "id": "editing_structure",
      "type": "skill",
      "description": "Improve document organization",
      "keywords": [
        "restructure text",
        "gliederung",
        "umstrukturieren"
      ],
      "prompt": "Group related ideas, remove repetition and order sections for the intended reader."
    },
    {
      "id": "fact_preserving_rewrite",
      "type": "skill",
      "description": "Rewrite without factual drift",
      "keywords": [
        "rewrite this",
        "umschreiben",
        "paraphrase"
      ],
      "prompt": "Preserve all original constraints, facts, dates and negations; do not invent examples presented as facts."
    },
    {
      "id": "bilingual_terminology",
      "type": "skill",
      "description": "Preserve specialized terms in translation",
      "keywords": [
        "technical translation",
        "fach\u00fcbersetzung",
        "terminologie"
      ],
      "prompt": "Use consistent domain terminology across languages and avoid false friends or mistranslated units."
    },
    {
      "id": "story_dialogue",
      "type": "skill",
      "description": "Improve fictional dialogue",
      "keywords": [
        "dialogue",
        "dialog",
        "sprechweise",
        "character voice",
        "人物对话",
        "角色对话"
      ],
      "prompt": "Give characters distinct voices and motivations; use subtext where appropriate and avoid exposition disguised as speech."
    },
    {
      "id": "story_pacing",
      "type": "skill",
      "description": "Tune narrative pacing and tension",
      "keywords": [
        "pacing",
        "spannung",
        "dramaturgie",
        "plot pacing",
        "故事节奏",
        "叙事节奏"
      ],
      "prompt": "Vary scene density and tension intentionally; avoid rushing critical emotional transitions."
    },
    {
      "id": "story_worldbuilding",
      "type": "skill",
      "description": "Keep fictional world rules consistent",
      "keywords": [
        "worldbuilding",
        "weltenbau",
        "fantasy world",
        "science fiction setting",
        "世界观设定",
        "世界构建"
      ],
      "prompt": "Maintain internally consistent technology, geography and rules; explain only what the scene needs."
    },
    {
      "id": "story_characterization",
      "type": "skill",
      "description": "Develop believable characters",
      "keywords": [
        "character arc",
        "charakterentwicklung",
        "protagonist",
        "角色塑造",
        "人物成长",
        "人物塑造"
      ],
      "prompt": "Give characters plausible goals, constraints and changes; avoid purely declarative personality labels."
    },
    {
      "id": "story_pov",
      "type": "skill",
      "description": "Maintain point of view and narrative distance",
      "keywords": [
        "point of view",
        "erz\u00e4hlperspektive",
        "first person narrator"
      ],
      "prompt": "Keep viewpoint and tense consistent unless a deliberate transition is requested."
    },
    {
      "id": "story_genre",
      "type": "skill",
      "description": "Honor genre conventions without clich\u00e9s",
      "keywords": [
        "genre",
        "thriller",
        "romance story",
        "horror story"
      ],
      "prompt": "Use the chosen genres expectations as support, not a formula; favor distinctive details over stock tropes."
    },
    {
      "id": "story_continuity",
      "type": "skill",
      "description": "Track story facts across long texts",
      "keywords": [
        "plot hole",
        "continuity",
        "kontinuit\u00e4t",
        "widerspruch story",
        "剧情连贯",
        "情节漏洞"
      ],
      "prompt": "Track established events, timing and relationships; avoid contradicting earlier material."
    },
    {
      "id": "table_formatting",
      "type": "skill",
      "description": "Use compact comparison tables when useful",
      "keywords": [
        "table",
        "tabellarisch",
        "tabelle",
        "comparison chart"
      ],
      "prompt": "Use tables for aligned criteria and avoid rows that repeat full paragraphs. Do not sacrifice accessibility for format."
    },
    {
      "id": "step_by_step",
      "type": "skill",
      "description": "Provide ordered actionable steps",
      "keywords": [
        "step by step",
        "schritt f\u00fcr schritt",
        "how to",
        "anleitung"
      ],
      "prompt": "Number only meaningful actions, state prerequisites and surface irreversible steps before performing them."
    },
    {
      "id": "checklist",
      "type": "skill",
      "description": "Produce an actionable checklist",
      "keywords": [
        "checklist",
        "checkliste",
        "pr\u00fcfliste"
      ],
      "prompt": "Make checks observable and independent; avoid generic items that cannot be verified."
    },
    {
      "id": "timeline",
      "type": "skill",
      "description": "Describe chronological events clearly",
      "keywords": [
        "timeline",
        "zeitstrahl",
        "chronologie",
        "chronological"
      ],
      "prompt": "Separate confirmed dates from relative order and keep event dependencies explicit."
    },
    {
      "id": "requirements_traceability",
      "type": "skill",
      "description": "Map requirements to evidence and tests",
      "keywords": [
        "traceability",
        "anforderungsnachweis",
        "requirements mapping"
      ],
      "prompt": "Map each requirement to implementation and a verifiable test or evidence source; label missing coverage."
    },
    {
      "id": "issue_triage",
      "type": "skill",
      "description": "Triage defects and scope fixes",
      "keywords": [
        "bug report",
        "issue triage",
        "fehlerbericht",
        "ticket"
      ],
      "prompt": "Summarize reproduction, impact, suspected cause and next diagnostic action; avoid declaring unverified causes."
    },
    {
      "id": "root_cause",
      "type": "skill",
      "description": "Distinguish root causes from symptoms",
      "keywords": [
        "root cause",
        "ursachenanalyse",
        "five whys",
        "根本原因",
        "根因分析"
      ],
      "prompt": "Separate symptoms, contributing conditions and root cause evidence. Propose a falsifiable hypothesis."
    },
    {
      "id": "postmortem",
      "type": "skill",
      "description": "Analyze incidents without blame",
      "keywords": [
        "postmortem",
        "incident review",
        "st\u00f6rungsanalyse"
      ],
      "prompt": "Construct a factual timeline and highlight systemic safeguards, not unsupported personal blame."
    },
    {
      "id": "stakeholder_summary",
      "type": "skill",
      "description": "Adjust detail for stakeholders",
      "keywords": [
        "stakeholder update",
        "status update",
        "projektstatus"
      ],
      "prompt": "Lead with status, blockers, decisions and ownership; avoid unnecessary technical minutiae."
    },
    {
      "id": "budgeting",
      "type": "skill",
      "description": "Track costs and tradeoffs",
      "keywords": [
        "budget",
        "kostenplan",
        "cost estimate",
        "budgetierung"
      ],
      "prompt": "Expose assumptions, fixed versus variable costs and uncertainty; avoid exact totals without data."
    },
    {
      "id": "time_estimation",
      "type": "skill",
      "description": "Estimate effort with uncertainty",
      "keywords": [
        "estimate effort",
        "aufwand sch\u00e4tzen",
        "time estimate"
      ],
      "prompt": "Use ranges with identified dependencies and describe what could invalidate the estimate."
    },
    {
      "id": "project_dependencies",
      "type": "skill",
      "description": "Track project prerequisites and blockers",
      "keywords": [
        "dependencies",
        "abh\u00e4ngigkeiten",
        "blockers",
        "voraussetzungen"
      ],
      "prompt": "Identify sequencing constraints and critical blockers rather than listing every task as equal."
    },
    {
      "id": "interview_prep",
      "type": "skill",
      "description": "Prepare structured interview questions",
      "keywords": [
        "interview prep",
        "vorstellungsgespr\u00e4ch",
        "interview vorbereiten"
      ],
      "prompt": "Focus on evidence-based examples and relevant questions; avoid rehearsed claims that do not fit actual experience."
    },
    {
      "id": "learning_plan",
      "type": "skill",
      "description": "Build a realistic learning curriculum",
      "keywords": [
        "learning plan",
        "lernplan",
        "studienplan",
        "course plan"
      ],
      "prompt": "Sequence prerequisites and practice, with small checkpoints to verify understanding."
    },
    {
      "id": "socratic_questions",
      "type": "skill",
      "description": "Use guided questions to develop understanding",
      "keywords": [
        "socratic",
        "leitfragen",
        "frage mich",
        "guided questions"
      ],
      "prompt": "Ask a focused question that exposes the next reasoning step without hiding necessary information."
    },
    {
      "id": "recipe_precision",
      "type": "skill",
      "description": "Make recipes reproducible and practical",
      "keywords": [
        "baking recipe",
        "backrezept",
        "grams",
        "gramm",
        "oven temperature"
      ],
      "prompt": "Use consistent quantities, temperature and timing; note which tolerances depend on equipment."
    },
    {
      "id": "food_safety",
      "type": "skill",
      "description": "Safeguard important cooking safety information",
      "keywords": [
        "food safety",
        "lebensmittelsicherheit",
        "raw chicken",
        "rohes fleisch"
      ],
      "prompt": "Separate safety-critical temperatures and storage from flexible cooking preferences."
    },
    {
      "id": "travel_logistics",
      "type": "skill",
      "description": "Identify travel transfer and timing constraints",
      "keywords": [
        "train connection",
        "flugverbindung",
        "umsteigen",
        "reisezeit"
      ],
      "prompt": "Account for connections, buffer times and local constraints; do not claim schedules are live without checking."
    },
    {
      "id": "consumer_comparison",
      "type": "skill",
      "description": "Evaluate products against needs",
      "keywords": [
        "product comparison",
        "produktempfehlung",
        "kaufberatung"
      ],
      "prompt": "Prioritize user constraints and tradeoffs; distinguish objective specifications from preferences."
    },
    {
      "id": "troubleshooting_hardware",
      "type": "skill",
      "description": "Troubleshoot physical computer hardware",
      "keywords": [
        "hardware issue",
        "bios",
        "pc startet nicht",
        "gpu issue"
      ],
      "prompt": "Start with safe checks and distinguish power, thermal, driver and hardware evidence."
    },
    {
      "id": "troubleshooting_network",
      "type": "skill",
      "description": "Diagnose connectivity and protocols",
      "keywords": [
        "network issue",
        "netzwerkproblem",
        "ssh timeout",
        "dns fails"
      ],
      "prompt": "Separate DNS, routing, transport, authentication and application-layer failures with minimal tests."
    },
    {
      "id": "troubleshooting_linux",
      "type": "skill",
      "description": "Debug Linux systems and services",
      "keywords": [
        "ubuntu",
        "linux issue",
        "systemd",
        "kernel log"
      ],
      "prompt": "Prefer reproducible terminal checks, preserve logs and note when a command changes persistent system state."
    },
    {
      "id": "troubleshooting_windows",
      "type": "skill",
      "description": "Debug Windows systems and services",
      "keywords": [
        "windows issue",
        "event viewer",
        "powershell issue"
      ],
      "prompt": "Consider Windows path, permissions and service semantics; suggest low-risk diagnostics first."
    },
    {
      "id": "computer_vision",
      "type": "skill",
      "description": "Image interpretation and geometric uncertainty",
      "keywords": [
        "computer vision",
        "bildanalyse",
        "object detection",
        "segmentation"
      ],
      "prompt": "Distinguish directly visible evidence from inferred scene properties. State ambiguity caused by occlusion, viewpoint and resolution."
    },
    {
      "id": "image_generation",
      "type": "skill",
      "description": "Writing effective visual generation prompts",
      "keywords": [
        "image prompt",
        "generate image",
        "bild generieren",
        "prompt f\u00fcr bild"
      ],
      "prompt": "Specify subject, action, composition, lighting and constraints clearly while avoiding contradictory visual instructions."
    },
    {
      "id": "image_editing",
      "type": "skill",
      "description": "Targeted changes while preserving an image",
      "keywords": [
        "edit photo",
        "bild bearbeiten",
        "inpainting",
        "outpainting"
      ],
      "prompt": "Specify which regions change and which visual identities, lighting or geometry must remain consistent."
    },
    {
      "id": "audio_analysis",
      "type": "skill",
      "description": "Interpreting audio features and recordings",
      "keywords": [
        "audio signal",
        "audio analysieren",
        "waveform",
        "recording"
      ],
      "prompt": "Distinguish recorded evidence from interpretation; state sample-rate and noise limitations when relevant."
    },
    {
      "id": "video_synthesis",
      "type": "skill",
      "description": "Frame continuity and temporal coherence",
      "keywords": [
        "video generation",
        "video erzeugen",
        "animation prompt",
        "motion clip"
      ],
      "prompt": "Describe motion, temporal continuity and fixed scene elements; avoid impossible simultaneous camera or object changes."
    },
    {
      "id": "ml_evaluation",
      "type": "skill",
      "description": "Fair machine learning model evaluation",
      "keywords": [
        "ml benchmark",
        "machine learning evaluation",
        "modellvergleich benchmark"
      ],
      "prompt": "Distinguish training from evaluation data and report leakage risks, variance and representative failure categories."
    },
    {
      "id": "llm_quantization",
      "type": "skill",
      "description": "Model quantization quality and memory tradeoffs",
      "keywords": [
        "gguf",
        "quantization",
        "quantisierung",
        "4bit model"
      ],
      "prompt": "State format, precision and runtime assumptions. Separate file size, resident memory and measured quality loss."
    },
    {
      "id": "inference_optimization",
      "type": "skill",
      "description": "Local inference latency and throughput",
      "keywords": [
        "llama.cpp",
        "vllm",
        "inference speed",
        "tokens per second"
      ],
      "prompt": "Separate prefill, decode and memory costs. Compare equivalent workloads and context lengths before concluding a speedup."
    },
    {
      "id": "gpu_memory_pressure",
      "type": "skill",
      "description": "Unified-memory and VRAM pressure reasoning",
      "keywords": [
        "vram",
        "uma memory",
        "gpu oom",
        "memory oversubscription"
      ],
      "prompt": "Distinguish address-space reservation, physical allocation, residency and shared-memory contention; do not infer limits from nominal capacity alone."
    },
    {
      "id": "retrieval_quality",
      "type": "skill",
      "description": "Retrieval and knowledge grounding",
      "keywords": [
        "rag system",
        "retrieval",
        "search index",
        "vector retrieval"
      ],
      "prompt": "Evaluate candidate recall and relevance separately. Do not equate retrieved text with a verified fact."
    },
    {
      "id": "prompt_composition",
      "type": "skill",
      "description": "Modular prompt and instruction design",
      "keywords": [
        "system prompt",
        "prompt architecture",
        "instruction loader",
        "prompt routing"
      ],
      "prompt": "Keep user intent separate from control instructions, define precedence and avoid redundant or conflicting guidance."
    },
    {
      "id": "multi_agent_coordination",
      "type": "skill",
      "description": "Coordinating multiple assistants or delegated tasks",
      "keywords": [
        "multi agent",
        "delegate tasks",
        "subagent",
        "orchestrator"
      ],
      "prompt": "Define clear interfaces, budgets and result ownership; avoid duplicated work and unsupported completion claims."
    },
    {
      "id": "code_diff_review",
      "type": "skill",
      "description": "Read diffs and identify regression surfaces",
      "keywords": [
        "git diff",
        "patch review",
        "changes in patch",
        "code changes"
      ],
      "prompt": "Relate each concern to a changed line and an observable failure. Distinguish existing issues from regressions."
    },
    {
      "id": "distributed_systems",
      "type": "skill",
      "description": "Distributed consistency and fault tolerance",
      "keywords": [
        "distributed systems",
        "microservices",
        "consensus",
        "replikation"
      ],
      "prompt": "Identify failure domains, ordering guarantees and retry semantics; avoid assuming remote actions are atomic."
    },
    {
      "id": "reliability_engineering",
      "type": "skill",
      "description": "Reliability and availability of systems",
      "keywords": [
        "sre",
        "reliability",
        "availability",
        "slo"
      ],
      "prompt": "Focus on concrete failure modes, detection, safe degradation and recovery time rather than generic best practices."
    },
    {
      "id": "network_security",
      "type": "skill",
      "description": "Network trust and authorization boundaries",
      "keywords": [
        "tls",
        "authentication",
        "authorization",
        "network security"
      ],
      "prompt": "Distinguish identity, transport security and permission checks. Avoid trusting network locality as authorization."
    },
    {
      "id": "numerical_integration",
      "type": "skill",
      "description": "Numerical time integration and error control",
      "keywords": [
        "ode solver",
        "time step",
        "zeitintegration",
        "integrator"
      ],
      "prompt": "State stability and local-error assumptions, compare step sizes and separate physical damping from numerical damping."
    },
    {
      "id": "constraint_optimization",
      "type": "skill",
      "description": "Constrained numerical optimization",
      "keywords": [
        "linear programming",
        "quadratic program",
        "optimization constraints"
      ],
      "prompt": "Identify objective, feasible region and constraint qualification; distinguish infeasibility from optimizer failure."
    },
    {
      "id": "chemical_safety",
      "type": "skill",
      "description": "General chemical hazard awareness",
      "keywords": [
        "chemical handling",
        "chemikalien sicherheit",
        "hazardous reaction"
      ],
      "prompt": "Surface hazards and appropriate protective assumptions without giving unsafe instructions or implying laboratory verification."
    },
    {
      "id": "pharmacology",
      "type": "skill",
      "description": "Medication-related mechanisms and uncertainty",
      "keywords": [
        "pharmacology",
        "medication interaction",
        "medikament wechselwirkung"
      ],
      "prompt": "Distinguish mechanism, observed evidence and individual risk; avoid prescribing or claiming compatibility without reliable information."
    },
    {
      "id": "physiology",
      "type": "skill",
      "description": "Human physiological processes",
      "keywords": [
        "physiology",
        "physiologie",
        "blutkreislauf",
        "organfunktion"
      ],
      "prompt": "Explain interacting body systems and avoid oversimplified one-cause explanations for multifactorial symptoms."
    },
    {
      "id": "ecology",
      "type": "skill",
      "description": "Ecosystems and interdependent effects",
      "keywords": [
        "ecosystem",
        "\u00f6kologie",
        "food web",
        "artenvielfalt"
      ],
      "prompt": "Consider feedback, habitat and timescale effects; avoid inferring ecosystem-wide outcomes from a single species."
    },
    {
      "id": "epidemiology",
      "type": "skill",
      "description": "Population health studies and risk",
      "keywords": [
        "epidemiology",
        "epidemiologie",
        "infection rate",
        "incidence"
      ],
      "prompt": "Distinguish incidence, prevalence, relative risk and confounding; avoid extrapolating group effects to individuals."
    },
    {
      "id": "public_policy",
      "type": "skill",
      "description": "Explain policy tradeoffs neutrally",
      "keywords": [
        "public policy",
        "policy impact",
        "\u00f6ffentliche politik",
        "regulierung"
      ],
      "prompt": "Separate stated objectives, mechanisms and evidence of effects; present competing stakeholder interests fairly."
    },
    {
      "id": "historical_context",
      "type": "skill",
      "description": "Historical chronology and contested sources",
      "keywords": [
        "history",
        "geschichte jahrhundert",
        "historical timeline",
        "historische quelle"
      ],
      "prompt": "Anchor events to reliable dates and distinguish primary sources, scholarly interpretation and historical uncertainty."
    },
    {
      "id": "geopolitics",
      "type": "skill",
      "description": "International relations and changing context",
      "keywords": [
        "geopolitics",
        "international relations",
        "au\u00dfenpolitik",
        "sanctions"
      ],
      "prompt": "Distinguish official positions, independently verified events and predictions; avoid partisan framing."
    },
    {
      "id": "linguistics",
      "type": "skill",
      "description": "Language structure and usage",
      "keywords": [
        "linguistics",
        "linguistik",
        "syntax",
        "wortherkunft"
      ],
      "prompt": "Distinguish descriptive language use from formal conventions and note dialect or register when relevant."
    },
    {
      "id": "philosophy",
      "type": "skill",
      "description": "Clarify concepts and philosophical arguments",
      "keywords": [
        "philosophy",
        "philosophie",
        "epistemology",
        "erkenntnistheorie"
      ],
      "prompt": "Define contested terms and compare argument structures without presenting one school as settled fact."
    },
    {
      "id": "ethics_analysis",
      "type": "skill",
      "description": "Analyze ethical dimensions without moralizing",
      "keywords": [
        "ethics",
        "ethik",
        "ethical dilemma",
        "moral dilemma"
      ],
      "prompt": "Identify affected parties, competing values and consequences; avoid pretending there is a single objective answer."
    },
    {
      "id": "proof_verification",
      "type": "skill",
      "description": "Assess mathematical proof validity",
      "keywords": [
        "proof",
        "beweis",
        "prove theorem",
        "mathematischer beweis"
      ],
      "prompt": "Check quantifiers, edge cases and each implication. State exactly where a proof remains incomplete."
    },
    {
      "id": "linear_algebra",
      "type": "skill",
      "description": "Matrices, vectors and linear systems",
      "keywords": [
        "linear algebra",
        "matrix",
        "matrixrechnung",
        "eigenvalue"
      ],
      "prompt": "Track dimensions, basis assumptions and conditioning; distinguish algebraic solutions from numerical approximations."
    },
    {
      "id": "calculus",
      "type": "skill",
      "description": "Differentiation and integration methods",
      "keywords": [
        "derivative",
        "ableitung",
        "integral",
        "differenzieren"
      ],
      "prompt": "State domain and differentiability assumptions; verify derivatives by substitution or elementary checks when possible."
    },
    {
      "id": "differential_equations",
      "type": "skill",
      "description": "Ordinary and partial differential equations",
      "keywords": [
        "differential equation",
        "differentialgleichung",
        "pde",
        "ode"
      ],
      "prompt": "Identify initial and boundary conditions, existence assumptions and qualitative behavior before solving."
    },
    {
      "id": "data_visualization",
      "type": "skill",
      "description": "Visual encodings and chart correctness",
      "keywords": [
        "plot data",
        "diagramm",
        "visualisierung",
        "chart design"
      ],
      "prompt": "Choose axes and scales that preserve quantitative meaning; highlight uncertainty, missing data and misleading truncation."
    },
    {
      "id": "time_series_analysis",
      "type": "skill",
      "description": "Temporal trends, seasonality and changes",
      "keywords": [
        "time series",
        "zeitreihe",
        "seasonality",
        "trend analysis"
      ],
      "prompt": "Distinguish trend, seasonality, sampling artifacts and non-stationarity; avoid forecasting beyond supported evidence."
    },
    {
      "id": "experimental_statistics",
      "type": "skill",
      "description": "Design of experiments and statistical power",
      "keywords": [
        "design of experiments",
        "doe",
        "statistical power",
        "versuchsplanung"
      ],
      "prompt": "Define treatment and control, randomization, replication and power before interpreting effects."
    },
    {
      "id": "spreadsheet_logic",
      "type": "skill",
      "description": "Spreadsheet formulas and cell references",
      "keywords": [
        "excel formula",
        "spreadsheet formula",
        "tabellenkalkulation",
        "zellbezug"
      ],
      "prompt": "Use clear formulas, correct relative/absolute references and explicit handling of blanks and errors."
    },
    {
      "id": "procurement",
      "type": "skill",
      "description": "Selecting suppliers and commercial options",
      "keywords": [
        "supplier comparison",
        "lieferantenvergleich",
        "procurement",
        "beschaffung"
      ],
      "prompt": "Compare technical fit, risk, service and total lifecycle cost, not only unit price."
    },
    {
      "id": "process_optimization",
      "type": "skill",
      "description": "Industrial process parameters and experiments",
      "keywords": [
        "process optimization",
        "prozessoptimierung",
        "parameterstudie"
      ],
      "prompt": "Identify controllable inputs, measured outputs and process constraints; propose small informative experiments."
    },
    {
      "id": "manufacturing_quality",
      "type": "skill",
      "description": "Quality control and production variation",
      "keywords": [
        "quality control",
        "fertigungsqualit\u00e4t",
        "cpk",
        "prozessf\u00e4higkeit"
      ],
      "prompt": "Distinguish measurement-system variation from process variation and tie recommendations to observed defects."
    },
    {
      "id": "gear_engineering",
      "type": "skill",
      "description": "Gear meshing, loads and failure",
      "keywords": [
        "gear mesh",
        "zahnrad",
        "ritzellast",
        "getriebe"
      ],
      "prompt": "Distinguish macrogeometry, microgeometry, lubrication and load distribution; connect failure mode to evidence."
    },
    {
      "id": "fatigue_mechanics",
      "type": "skill",
      "description": "Fatigue failure and cumulative damage",
      "keywords": [
        "fatigue",
        "erm\u00fcdung",
        "lebensdauer berechnen",
        "rissbildung"
      ],
      "prompt": "Distinguish initiation and propagation, mean stress and cycle counting; note load spectrum and material assumptions."
    },
    {
      "id": "corrosion_mechanisms",
      "type": "skill",
      "description": "Corrosion mechanisms and environments",
      "keywords": [
        "corrosion",
        "korrosion",
        "oxidation",
        "rust prevention"
      ],
      "prompt": "Tie likely mechanism to chemistry, environment and material pairing; avoid universal corrosion-rate claims."
    },
    {
      "id": "surface_finishing",
      "type": "skill",
      "description": "Surface finishing, roughness and process effects",
      "keywords": [
        "surface finish",
        "oberfl\u00e4chenfinish",
        "rauheit",
        "polishing"
      ],
      "prompt": "Differentiate roughness metrics, topography, residual stress and process parameters; avoid equating lower roughness with universally better performance."
    },
    {
      "id": "mechanical_design",
      "type": "skill",
      "description": "Mechanical component sizing and tolerance",
      "keywords": [
        "mechanical design",
        "mechanische konstruktion",
        "toleranzkette"
      ],
      "prompt": "Specify loads, materials, manufacturing limits and safety factors; check tolerance stackups and fit."
    },
    {
      "id": "safety_engineering",
      "type": "skill",
      "description": "Engineering hazards and safety margins",
      "keywords": [
        "safety margin",
        "sicherheitsfaktor",
        "functional safety",
        "fail safe"
      ],
      "prompt": "Separate hazards, protective measures and residual risk; do not infer certification from a conceptual design."
    },
    {
      "id": "energy_systems",
      "type": "skill",
      "description": "Energy balances and system efficiency",
      "keywords": [
        "energy efficiency",
        "wirkungsgrad",
        "stromverbrauch",
        "energy balance"
      ],
      "prompt": "Define the system boundary and compare delivered useful energy rather than nominal component ratings."
    },
    {
      "id": "electrochemistry",
      "type": "skill",
      "description": "Electrode and electrochemical reactions",
      "keywords": [
        "electrochemistry",
        "elektrochemie",
        "electrode potential",
        "ion transport"
      ],
      "prompt": "Separate equilibrium potential, overpotential, transport and kinetic limits; state electrolyte assumptions."
    },
    {
      "id": "computational_chemistry",
      "type": "skill",
      "description": "Computer-based molecular and chemical estimates",
      "keywords": [
        "molecular dynamics",
        "quantenchemie",
        "density functional",
        "dft calculation"
      ],
      "prompt": "State approximation level, system model and convergence limits; never treat an unrun simulation as evidence."
    },
    {
      "id": "microbiology",
      "type": "skill",
      "description": "Microorganisms and experimental uncertainty",
      "keywords": [
        "microbiology",
        "mikrobiologie",
        "bacteria",
        "microbiome"
      ],
      "prompt": "Distinguish species-level facts from strain- and environment-specific effects; avoid overclaiming causality."
    },
    {
      "id": "education_assessment",
      "type": "skill",
      "description": "Construct fair assessment questions",
      "keywords": [
        "exam question",
        "pr\u00fcfungsfrage",
        "test questions",
        "assessment rubric"
      ],
      "prompt": "Align questions with learning objectives and grading criteria; avoid ambiguous distractors and hidden prerequisites."
    },
    {
      "id": "speech_style",
      "type": "skill",
      "description": "Polish spoken explanations and scripts",
      "keywords": [
        "spoken presentation",
        "gesprochen",
        "vorlesetext",
        "script for voice"
      ],
      "prompt": "Use short pronounceable sentences and clear transitions without losing technical accuracy."
    },
    {
      "id": "documentation_examples",
      "type": "skill",
      "description": "Document copy-paste runnable examples",
      "keywords": [
        "code sample",
        "example usage",
        "beispielcode",
        "api example"
      ],
      "prompt": "Use internally consistent names and paths, and explicitly mark assumptions or placeholders."
    },
    {
      "id": "scientific-reasoning",
      "type": "skill",
      "description": "Explain scientific causation and hypotheses",
      "keywords": [
        "warum",
        "scientific mechanism",
        "physikalisch",
        "biologisch"
      ],
      "prompt": "Differentiate established mechanisms, observations and hypotheses; justify causal claims and state when evidence is limited.",
      "priority": 10
    },
    {
      "id": "mathematics_discussion",
      "type": "skill",
      "description": "Discuss mathematics, math concepts and mathematical intuition without assuming a calculation",
      "keywords": [
        "math",
        "maths",
        "mathematics",
        "mathe",
        "mathematik",
        "mathematics discussion",
        "talk about math",
        "talk to me about math",
        "聊聊数学",
        "数学概念"
      ],
      "prompt": "For open-ended questions about mathematics, start with approachable ideas and ask which direction interests the user. Explain concepts and intuition; do not invent a numerical problem or force a step-by-step solution unless requested."
    }
  ]
}
)adaptive_catalog";
} // namespace lemon::adaptive_prompt
