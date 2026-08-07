import { z } from "zod";

/**
 * Response schemas mirror the contracts documented in the parent repository:
 *   docs/api/openai.md   — chat/completions, embeddings, models
 *   docs/api/lemonade.md — health, stats, system-stats, system-info
 * Unknown fields are tolerated so a newer Lemonade Server never breaks the app.
 */

export const loadedModelSchema = z
  .object({
    model_name: z.string(),
    checkpoint: z.string().optional(),
    type: z.string().optional(),
    device: z.string().optional(),
    recipe: z.string().optional(),
  })
  .passthrough();

export const healthSchema = z
  .object({
    status: z.string(),
    version: z.string().optional(),
    model_loaded: z.string().nullable().optional(),
    all_models_loaded: z.array(loadedModelSchema).optional(),
  })
  .passthrough();

export const modelSchema = z
  .object({
    id: z.string(),
    object: z.string().optional(),
    owned_by: z.string().optional(),
    checkpoint: z.string().optional(),
    recipe: z.string().optional(),
    size: z.number().optional(),
    max_context_window: z.number().optional(),
    downloaded: z.boolean().optional(),
    labels: z.array(z.string()).optional(),
  })
  .passthrough();

export const modelListSchema = z
  .object({
    object: z.string().optional(),
    data: z.array(modelSchema),
  })
  .passthrough();

export const chatCompletionSchema = z
  .object({
    id: z.string().optional(),
    model: z.string().optional(),
    choices: z
      .array(
        z
          .object({
            index: z.number().optional(),
            message: z
              .object({
                role: z.string().optional(),
                content: z.string().nullable().optional(),
              })
              .passthrough()
              .optional(),
            finish_reason: z.string().nullable().optional(),
          })
          .passthrough(),
      )
      .min(1),
    usage: z
      .object({
        prompt_tokens: z.number().optional(),
        completion_tokens: z.number().optional(),
        total_tokens: z.number().optional(),
      })
      .passthrough()
      .optional(),
  })
  .passthrough();

export const chatChunkSchema = z
  .object({
    choices: z
      .array(
        z
          .object({
            delta: z
              .object({
                role: z.string().optional(),
                content: z.string().nullable().optional(),
              })
              .passthrough()
              .optional(),
            finish_reason: z.string().nullable().optional(),
          })
          .passthrough(),
      )
      .optional(),
  })
  .passthrough();

export const embeddingResponseSchema = z
  .object({
    object: z.string().optional(),
    data: z
      .array(
        z
          .object({
            index: z.number().optional(),
            embedding: z.array(z.number()).min(1),
          })
          .passthrough(),
      )
      .min(1),
    model: z.string().optional(),
    usage: z
      .object({
        prompt_tokens: z.number().optional(),
        total_tokens: z.number().optional(),
      })
      .passthrough()
      .optional(),
  })
  .passthrough();

export const statsSchema = z
  .object({
    time_to_first_token: z.number().nullable().optional(),
    tokens_per_second: z.number().nullable().optional(),
    input_tokens: z.number().nullable().optional(),
    output_tokens: z.number().nullable().optional(),
    prompt_tokens: z.number().nullable().optional(),
  })
  .passthrough();

export const systemStatsSchema = z
  .object({
    cpu_percent: z.number().nullable().optional(),
    memory_gb: z.number().nullable().optional(),
    gpu_percent: z.number().nullable().optional(),
    vram_gb: z.number().nullable().optional(),
    npu_percent: z.number().nullable().optional(),
  })
  .passthrough();

const deviceSchema = z
  .object({
    name: z.string().optional(),
    available: z.boolean().optional(),
    family: z.string().optional(),
    vram_gb: z.number().optional(),
    cores: z.number().optional(),
    threads: z.number().optional(),
  })
  .passthrough();

/**
 * `/v1/system-info` uses human-readable top-level keys ("OS Version",
 * "Processor", "Physical Memory") — verified against docs/api/lemonade.md and
 * the assertions in test/server_endpoints.py of the parent repository.
 */
export const systemInfoSchema = z
  .object({
    "OS Version": z.string().optional(),
    Processor: z.string().optional(),
    "Physical Memory": z.string().optional(),
    "OEM System": z.string().optional(),
    devices: z
      .object({
        cpu: deviceSchema.optional(),
        amd_gpu: z.array(deviceSchema).optional(),
        amd_npu: deviceSchema.optional(),
        nvidia_gpu: z.array(deviceSchema).optional(),
      })
      .passthrough()
      .optional(),
  })
  .passthrough();

export type LemonadeHealth = z.infer<typeof healthSchema>;
export type LemonadeModel = z.infer<typeof modelSchema>;
export type LemonadeModelList = z.infer<typeof modelListSchema>;
export type LemonadeChatCompletion = z.infer<typeof chatCompletionSchema>;
export type LemonadeEmbeddingResponse = z.infer<typeof embeddingResponseSchema>;
export type LemonadeStats = z.infer<typeof statsSchema>;
export type LemonadeSystemStats = z.infer<typeof systemStatsSchema>;
export type LemonadeSystemInfo = z.infer<typeof systemInfoSchema>;
