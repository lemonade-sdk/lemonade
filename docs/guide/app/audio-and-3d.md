# Audio and 3D in the Lemonade App

[App overview](README.md)

Lemonade App treats speech input, speech output, generated audio, and 3D as distinct capabilities instead of overloading one generic audio mode.

| Capability | Recipes | Composer workflow | Endpoint |
| --- | --- | --- | --- |
| Transcription | `whispercpp`, `moonshine` | audio file or microphone -> text | `/api/v1/audio/transcriptions`, `/v1/realtime` |
| TTS | `kokoro`, `openmoss` | text -> spoken audio | `/api/v1/audio/speech` |
| Audio generation | `acestep`, `thinksound`, `openmoss` | prompt -> music or sound effect | `/api/v1/audio/generations` |
| 3D | `trellis` | image -> GLB, or text -> reference image -> GLB | `/api/v1/3d/generations` |

## ACE-Step and ThinkSound

ACE-Step exposes music duration, steps, seed, optional structured lyrics, and a vocal language. Empty lyrics generate an instrumental track. ThinkSound exposes duration, steps, CFG, and seed for sound-effect generation. Both return downloadable WAV output.

## OpenMOSS

OpenMOSS speech runs every voice workflow against the one selected model:

- **Plain:** synthesize text with an optional style instruction, sent as `voice`.
- **Describe voice:** send the description as `voice_design_description`. `OpenMOSS-TTS` and `MOSS-TTS-Local` ship a voice generator as a component: the server renders a short reference sample in the described voice, then speaks the text with it. The app leaves `voice` empty in this mode, so `voice` keeps its OpenAI-compatible meaning.
- **Clone WAV sample:** attach one validated WAV sample, sent as `reference_wav_b64`. The optional style note travels as `voice`, which directs delivery without changing the timbre.

The app no longer switches between a voice-design model and a speech model. The server runs that sequence inside the speech model's backend, which keeps the model loaded and selected for the whole request. The legacy `MOSS-VoiceGen` entry (labelled `voice-design`) only offers **Describe voice**, and its description goes in `voice`, because that model is itself the voice generator.

**Generation parameters** expands the speech sampling fields: audio and text temperature, top-p, top-k, repetition penalty, and speed. A blank field is not sent, so the backend applies the defaults for that model's architecture. `OpenMOSS-TTS` and `MOSS-TTS-Local` use different defaults.

`MOSS-SoundEffect` is an audio-generation model. On top of duration, steps, CFG, and seed, it has sigma-shift and negative-prompt controls. The registry declares no defaults for it, so the composer starts from the engine's own values (100 steps, CFG 4.0, sigma shift 5.0) instead of the generic sound-effect numbers. Recipe options still take precedence when a model declares them. The seed is unsigned and `0` is a real seed, so a blank or `-1` seed draws a random 32-bit value; otherwise every generation would render the same clip.

Audio-generation and OpenMOSS voice settings reset to the model's defaults when you select a different model. Values you change while the model's options are still loading are kept.

## TRELLIS

The 3D composer supports:

- **Image -> 3D:** upload one reference image and reconstruct it directly.
- **Text -> image -> 3D:** select a downloaded image model; Lemonade App renders a reconstruction-friendly reference image, reloads TRELLIS, then reconstructs the mesh.
- Cascade resolution `512`, `1024`, or `1536`, background removal, and seed controls.
- Interactive GLB preview and direct GLB download.
- Local binary STL export for printing-oriented geometry workflows. STL intentionally contains geometry only; materials and textures remain in GLB.


The STL converter supports embedded glTF 2.0 triangle, triangle-strip, and triangle-fan primitives, node transforms, indexed/non-indexed geometry, and sparse accessors. Compressed geometry is left as GLB and reports a clear export error rather than producing a damaged STL.
