# Audio generation and 3D modes

GUI3 treats speech input, speech output, generated audio, and 3D as distinct capabilities instead of overloading one generic audio mode.

| Capability | Recipes | Composer workflow | Endpoint |
| --- | --- | --- | --- |
| Transcription | `whispercpp`, `moonshine` | audio file or microphone -> text | `/api/v1/audio/transcriptions`, `/v1/realtime` |
| TTS | `kokoro`, `openmoss` | text -> spoken audio | `/api/v1/audio/speech` |
| Audio generation | `acestep`, `thinksound`, `openmoss` | prompt -> music or sound effect | `/api/v1/audio/generations` |
| 3D | `trellis` | image -> GLB, or text -> reference image -> GLB | `/api/v1/3d/generations` |

## ACE-Step and ThinkSound

ACE-Step exposes music duration, steps, seed, optional structured lyrics, and a vocal language. Empty lyrics generate an instrumental track. ThinkSound exposes duration, steps, CFG, and seed for sound-effect generation. Both return downloadable WAV output.

## OpenMOSS

OpenMOSS speech has two composer modes, both against a single loaded model:

- **Describe voice:** send the description as `voice_design_description`. The backend invents a voice matching it and speaks the text in that voice.
- **Clone WAV sample:** attach one validated WAV sample, sent as `reference_wav_b64`. The optional style note travels as `voice`, so delivery can be directed without changing the timbre.

Voice design is no longer a second model the GUI switches to. The voice generator ships as a component of the speech model, and the backend now drives the sequence the GUI used to drive by hand: it takes the speech model down, brings the voice generator up to render one reference sample, tears it down, and brings the speech model back. Only ever one model at a time, so any card that can run the speech model can design a voice for it. The sample is cached per description, so repeating a description costs nothing.

`MOSS-SoundEffect` is an audio-generation model rather than a speech one, exposing steps, CFG, sigma shift, a negative prompt, and a seed. Its seed is unsigned with no random sentinel — `0` is a real seed — so the composer draws a random one whenever the seed box is left blank.

## TRELLIS

The 3D composer supports:

- **Image -> 3D:** upload one reference image and reconstruct it directly.
- **Text -> image -> 3D:** select a downloaded image model; GUI3 renders a reconstruction-friendly reference image, reloads TRELLIS, then reconstructs the mesh.
- Cascade resolution `512`, `1024`, or `1536`, background removal, and seed controls.
- Interactive GLB preview and direct GLB download.
- Local binary STL export for printing-oriented geometry workflows. STL intentionally contains geometry only; materials and textures remain in GLB.

The 3D result UI and the vendored `model-viewer` bundle are compiled into one lazy webpack chunk that is loaded only after a model has been generated. Normal application startup therefore does not execute the viewer, while webpack derives the chunk URL from the actual main-bundle location for both `/` and `/web-app/` deployments. This mirrors the proven GUI2 module integration and avoids a separate runtime script path.

The STL converter supports embedded glTF 2.0 triangle, triangle-strip, and triangle-fan primitives, node transforms, indexed/non-indexed geometry, and sparse accessors. Compressed geometry is left as GLB and reports a clear export error rather than producing a damaged STL.
