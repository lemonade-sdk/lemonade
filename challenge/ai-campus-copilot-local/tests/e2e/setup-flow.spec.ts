/**
 * End-to-end test of the Lemonade integration path with a MOCKED server.
 *
 * Every /api/lemonade/** call is intercepted, so this proves the app's own
 * wiring — connection state, dynamic model discovery, filtering, persistence —
 * without needing a real Lemonade Server. It does NOT prove real inference
 * works; see tests/README.md for the live-server checklist.
 */
import { expect, test, type Page } from "@playwright/test";

const MODELS = {
  object: "list",
  data: [
    {
      id: "Qwen3-0.6B-GGUF",
      object: "model",
      owned_by: "lemonade",
      checkpoint: "unsloth/Qwen3-0.6B-GGUF:Q4_0",
      recipe: "llamacpp",
      size: 0.38,
      max_context_window: 40960,
      downloaded: true,
      labels: ["reasoning"],
    },
    {
      id: "nomic-embed-text-v1-GGUF",
      object: "model",
      owned_by: "lemonade",
      checkpoint: "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
      recipe: "llamacpp",
      size: 0.08,
      downloaded: true,
      labels: ["embeddings"],
    },
    {
      id: "Whisper-Large-v3-Turbo",
      object: "model",
      owned_by: "lemonade",
      recipe: "whispercpp",
      downloaded: true,
      labels: ["transcription"],
    },
  ],
};

const HEALTH = {
  status: "ok",
  version: "9.3.3",
  model_loaded: null,
  all_models_loaded: [],
};

const SYSTEM_INFO = {
  "OS Version": "Windows-11-10.0.26100-SP0",
  Processor: "AMD Ryzen AI 9 HX 370",
  "Physical Memory": "32.0 GB",
  devices: { cpu: { name: "AMD Ryzen AI 9 HX 370", cores: 12, threads: 24, available: true } },
};

async function mockLemonade(page: Page, options: { serverUp?: boolean } = {}) {
  const serverUp = options.serverUp ?? true;

  await page.route("**/api/lemonade/**", async (route) => {
    const url = route.request().url();

    if (!serverUp) {
      await route.fulfill({
        status: 503,
        contentType: "application/json",
        body: JSON.stringify({
          error: true,
          kind: "server_unavailable",
          message: "Could not reach Lemonade Server. Start it and confirm the URL is correct.",
        }),
      });
      return;
    }

    const body = url.includes("/health")
      ? HEALTH
      : url.includes("/models")
        ? MODELS
        : url.includes("/system-info")
          ? SYSTEM_INFO
          : {};

    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify(body),
    });
  });
}

test("shows recovery guidance when Lemonade Server is unreachable", async ({ page }) => {
  await mockLemonade(page, { serverUp: false });
  await page.goto("/setup");

  await expect(page.getByRole("heading", { name: /Lemonade Server is not reachable/i })).toBeVisible();
  await expect(page.getByText(/Start Lemonade Server, then press retry/i)).toBeVisible();
  await expect(page.getByText("Setup incomplete")).toBeVisible();

  // The status must never optimistically read as connected.
  await expect(page.getByRole("heading", { name: /Connected to Lemonade Server/i })).toHaveCount(0);
});

test("discovers models dynamically, filters them by capability, and persists the selection", async ({
  page,
}) => {
  await mockLemonade(page);
  await page.goto("/setup");

  await expect(page.getByRole("heading", { name: /Connected to Lemonade Server/i })).toBeVisible();
  await expect(page.getByRole("definition").filter({ hasText: /^9\.3\.3$/ })).toBeVisible();
  await expect(page.getByText("Windows-11-10.0.26100-SP0")).toBeVisible();

  const chatGroup = page.getByRole("radiogroup", { name: "Chat model" });
  const embeddingGroup = page.getByRole("radiogroup", { name: "Embedding model" });

  // Only the text-generation model is offered for chat; the transcription and
  // embedding models are filtered out.
  await expect(chatGroup.getByRole("radio")).toHaveCount(1);
  await expect(chatGroup.getByRole("radio", { name: /Qwen3-0\.6B-GGUF/ })).toBeVisible();

  await expect(embeddingGroup.getByRole("radio")).toHaveCount(1);
  await expect(embeddingGroup.getByRole("radio", { name: /nomic-embed-text-v1-GGUF/ })).toBeVisible();

  await expect(page.getByText("Setup incomplete")).toBeVisible();

  await chatGroup.getByRole("radio", { name: /Qwen3-0\.6B-GGUF/ }).click();
  await embeddingGroup.getByRole("radio", { name: /nomic-embed-text-v1-GGUF/ }).click();

  await expect(page.getByText("Ready to use")).toBeVisible();

  // Selections live in IndexedDB, so they must survive a reload.
  await page.reload();
  await expect(chatGroup.getByRole("radio", { name: /Qwen3-0\.6B-GGUF/ })).toHaveAttribute(
    "aria-checked",
    "true",
  );
  await expect(page.getByText("Ready to use")).toBeVisible();

  // The stored choice must reach the rest of the app.
  await page.goto("/dashboard");
  await expect(page.getByText("Chat: Qwen3-0.6B-GGUF")).toBeVisible();
  await expect(page.getByText("Embedding: nomic-embed-text-v1-GGUF")).toBeVisible();
});
