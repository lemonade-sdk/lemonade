/**
 * The descriptor metadata lemond publishes for every recipe in
 * `/v1/system-info` (src/cpp/server/system_info.cpp enriches each entry from
 * the C++ backend descriptor). The renderer reads recipe identity, modality,
 * and the option schema from that payload instead of a client-side table, so a
 * mock that omits it is not a response lemond can actually produce.
 *
 * Verbatim from the descriptors in src/cpp/include/lemon/backends/.
 */
const DESCRIPTORS: Record<string, Record<string, unknown>> = {
  llamacpp: {
    order: 0,
    display_name: 'Llama.cpp GPU',
    web_display_name: 'llama.cpp GPU',
    modality: 'Text generation',
    experimental: false,
    selectable_backend: true,
    uses_ctx_size: true,
    slot_policy: 'standard',
    options: [
      { name: 'llamacpp_backend', cli_flag: '--llamacpp', default: '', type_name: 'BACKEND', help: 'LlamaCpp backend to use', group: 'Llama.cpp Backend Options' },
      { name: 'llamacpp_device', cli_flag: '--llamacpp-device', default: '', type_name: 'DEVICES', help: 'Comma-separated list of accelerator devices to use (e.g. Vulkan0)', group: 'Llama.cpp Backend Options' },
      { name: 'llamacpp_args', cli_flag: '--llamacpp-args', default: '', type_name: 'ARGS', help: 'Custom arguments to pass to llama-server', group: 'Llama.cpp Backend Options' },
    ],
  },
  vllm: {
    order: 1,
    display_name: 'vLLM ROCm (experimental)',
    modality: 'Text generation',
    experimental: true,
    selectable_backend: true,
    uses_ctx_size: true,
    slot_policy: 'standard',
    options: [
      { name: 'vllm_backend', cli_flag: '--vllm', default: '', type_name: 'BACKEND', help: 'vLLM backend to use', group: 'vLLM Options' },
      { name: 'vllm_args', cli_flag: '--vllm-args', default: '', type_name: 'ARGS', help: 'Custom arguments to pass to the vLLM server', group: 'vLLM Options' },
    ],
  },
  kokoro: {
    order: 2,
    display_name: 'Kokoro',
    modality: 'Text-to-speech',
    experimental: false,
    selectable_backend: false,
    uses_ctx_size: false,
    slot_policy: 'standard',
    options: [],
  },
};

type RecipeEntry = Record<string, unknown>;

/** A `/v1/system-info` recipes map, each entry carrying its descriptor metadata. */
export function systemInfoRecipes(entries: Record<string, RecipeEntry>): Record<string, RecipeEntry> {
  const out: Record<string, RecipeEntry> = {};
  for (const [recipe, entry] of Object.entries(entries)) {
    if (!DESCRIPTORS[recipe]) throw new Error(`No descriptor fixture for recipe '${recipe}'`);
    out[recipe] = { ...DESCRIPTORS[recipe], ...entry };
  }
  return out;
}
