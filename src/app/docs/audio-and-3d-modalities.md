# Audio generation and 3D modes

GUI3 treats speech input, speech output, generated audio, and 3D as distinct capabilities instead of overloading one generic audio mode.

| Capability | Recipes | Composer workflow | Endpoint |
| --- | --- | --- | --- |
| Transcription | `whispercpp`, `moonshine` | audio file or microphone -> text | `/api/v1/audio/transcriptions`, `/v1/realtime` |
| TTS | `kokoro`, `openmoss` | text -> spoken audio | `/api/v1/audio/speech` |
| Audio generation | `acestep`, `thinksound`, `openmoss` | prompt -> music or sound effect | `/api/v1/audio/generations` |
| 3D | `trellis` | image -> GLB, or text -> reference image -> GLB | `/api/v1/3d/generations` |

## ACE-Step and ThinkSound

ACE-Step declares music duration, steps, seed, optional structured lyrics, and a vocal language; empty lyrics generate an instrumental track. ThinkSound declares duration, steps, CFG, and seed. Both return downloadable WAV output. A recipe offering a lyric sheet is the composer's only cue that it is generating music rather than a sound effect, which it uses for wording alone.

## Generation controls

The composer does not know which backend it is talking to. Every control under the prompt for `tts` and `audio-generation` is rendered from `recipes[<recipe>].generation_params[<mode>]` on `/v1/system-info`, and each value is sent in the request field the declaration names. A backend that adds a knob gets a control here without a client change; see "Generation parameters" in `docs/dev/backends-reference.md` for the declaration and its types.

Control values are seeded from the effective recipe options first, then the model's own `speech_defaults` / `audio_defaults`, then the parameter's declared default. A model shipping its own step count and CFG is therefore honoured rather than overwritten by a generic number.

Two declarations mean more than "render an input":

- **`exclusive_group`** marks alternatives. Members become a selector — a `voice_mode` group renders as a Voice mode dropdown — and only the chosen member is sent. A member typed `AUDIO_B64` turns on the attachment affordance with its declared `accept` filter and travels as base64 in its own field.
- **`random_sentinel`** settles what a blank seed box means. A backend that reads a sentinel is sent it; a backend whose seed is unsigned declares none, and the composer draws a seed inside the declared range instead of sending a value the backend would take literally.

When a server declares nothing for a recipe, the composer falls back to a generic duration/steps/CFG/seed set so it keeps working against an older `lemond`.

Model selection is independent of residency: a downloaded model the user picked stays picked even while it is not in `all_models_loaded`. Loading a backend can take minutes, eviction is a resource decision rather than a user one, and a backend that unloads and reloads itself mid-request — OpenMOSS designs a voice that way — is not something the composer should react to at all.

## OpenMOSS

OpenMOSS speech offers two voice sources against a single loaded model, both declared by the backend rather than built into the composer:

- **Describe voice:** the description travels as `voice_design_description`. The backend invents a matching voice and speaks the text in it.
- **Clone WAV sample:** one validated WAV sample travels as `reference_wav_b64`.

`voice` is a separate optional field in both cases, carrying a style note that directs delivery without changing the timbre. It keeps its OpenAI-compatible meaning: nothing the composer sends turns an ordinary `voice` value into a design request.

Voice design is not a second model the GUI switches to. The voice generator ships as a component of the speech model, and the backend drives the sequence the GUI used to drive by hand: it takes the speech model down, brings the voice generator up to render one reference sample, tears it down, and brings the speech model back. Only ever one model at a time, so any card that can run the speech model can design a voice for it, and the sample is cached per description.

`MOSS-SoundEffect` is an audio-generation model rather than a speech one. Its parameters — steps, CFG, sigma shift, a negative prompt, and an unsigned seed — arrive with the rest of the declaration.

## TRELLIS

The 3D composer supports:

- **Image -> 3D:** upload one reference image and reconstruct it directly.
- **Text -> image -> 3D:** select a downloaded image model; GUI3 renders a reconstruction-friendly reference image, reloads TRELLIS, then reconstructs the mesh.
- Cascade resolution `512`, `1024`, or `1536`, background removal, and seed controls.
- Interactive GLB preview and direct GLB download.
- Local binary STL export for printing-oriented geometry workflows. STL intentionally contains geometry only; materials and textures remain in GLB.

The 3D result UI and the vendored `model-viewer` bundle are compiled into one lazy webpack chunk that is loaded only after a model has been generated. Normal application startup therefore does not execute the viewer, while webpack derives the chunk URL from the actual main-bundle location for both `/` and `/web-app/` deployments. This mirrors the proven GUI2 module integration and avoids a separate runtime script path.

The STL converter supports embedded glTF 2.0 triangle, triangle-strip, and triangle-fan primitives, node transforms, indexed/non-indexed geometry, and sparse accessors. Compressed geometry is left as GLB and reports a clear export error rather than producing a damaged STL.
