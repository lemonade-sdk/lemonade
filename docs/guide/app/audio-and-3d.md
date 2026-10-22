# Audio and 3D in the Lemonade App

[App overview](README.md)

Lemonade App treats speech input, speech output, generated audio, and 3D as distinct capabilities instead of overloading one generic audio mode.

| Capability | Recipes | Composer workflow | Endpoint |
| --- | --- | --- | --- |
| Transcription | `whispercpp`, `moonshine` | audio file or microphone -> text | `/api/v1/audio/transcriptions`, `/v1/realtime` |
| TTS | `kokoro`, `openmoss` | text -> spoken audio | `/api/v1/audio/speech` |
| Audio generation | `acestep`, `thinksound` | prompt -> music or sound effect | `/api/v1/audio/generations` |
| 3D | `trellis` | image -> GLB, or text -> reference image -> GLB | `/api/v1/3d/generations` |

## ACE-Step and ThinkSound

ACE-Step exposes music duration, steps, seed, optional structured lyrics, and a vocal language. Empty lyrics generate an instrumental track. ThinkSound exposes duration, steps, CFG, and seed for sound-effect generation. Both return downloadable WAV output.

## OpenMOSS

OpenMOSS supports these voice workflows in the app:

- **Plain:** synthesize text with an optional style instruction.
- **Describe:** use `MOSS-VoiceGen` directly, or design a short reference voice and pass it to `OpenMOSS-TTS` when both models are installed.
- **Clone:** attach one validated WAV sample and synthesize through `OpenMOSS-TTS` with `reference_wav_b64`.

Voice-design and clone models are discovered from downloaded or loaded OpenMOSS models. Model switching is performed explicitly between the design and synthesis stages so the second request cannot accidentally run against the first backend instance.

## TRELLIS

The 3D composer supports:

- **Image -> 3D:** upload one reference image and reconstruct it directly.
- **Text -> image -> 3D:** select a downloaded image model; Lemonade App renders a reconstruction-friendly reference image, reloads TRELLIS, then reconstructs the mesh.
- Cascade resolution `512`, `1024`, or `1536`, background removal, and seed controls.
- Interactive GLB preview and direct GLB download.
- Local binary STL export for printing-oriented geometry workflows. STL intentionally contains geometry only; materials and textures remain in GLB.


The STL converter supports embedded glTF 2.0 triangle, triangle-strip, and triangle-fan primitives, node transforms, indexed/non-indexed geometry, and sparse accessors. Compressed geometry is left as GLB and reports a clear export error rather than producing a damaged STL.
