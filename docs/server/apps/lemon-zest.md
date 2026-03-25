# Lemon Zest

[Lemon Zest](https://github.com/phqen1x/lemon-zest) is a local, AI-powered image editor for object removal and inpainting. Using **Flux-2-Klein-4B** on a **Lemonade Server**, it provides a near-instant "Magic Eraser" experience — all running on your own hardware.

![License](https://img.shields.io/badge/license-MIT-blue)

## Screenshots
<div align="center">
   <br><em>Splash screen of Lemon Zest when you first open the application.</em></br>
   <img src="https://github.com/Phqen1x/lemon-zest/blob/main/assets/Splash.png" alt="Splash screen of Lemon Zest when you first open the application." width="600"/>
   
   <br><em>Lemon Zest inpainting selected regions of an image.</em></br>
   <img src="https://github.com/Phqen1x/lemon-zest/blob/main/assets/Editing.png" alt="Lemon Zest inpainting selected regions of an image." width="600"/>
</div>

## Features

- **Multiple Selection Tools** — Rectangle (R), Circle (C), Lasso (L), Brush (B), and Flood Fill (F) with adjustable tolerance
- **AI Inpainting** — Remove objects or fill regions using a custom prompt, with adjustable strength and step count
- **Crop-to-Mask Optimization** — Only the masked region is sent for inference, significantly reducing processing time
- **Superimpose** — Blend external images onto your canvas with optional prompt-guided blending
- **Undo/Redo** — Full history with up to 20 states (Ctrl+Z / Ctrl+Y)
- **Zoom & Pan** — Scroll wheel zoom with multiple preset levels (25%–400%)
- **Drag & Drop** — Load images by dragging them onto the window
- **Format Support** — PNG, JPG, JPEG, BMP, and WebP

## Prerequisites

Lemon Zest requires a running **Lemonade Server** to perform inpainting. The server handles model loading and inference locally.

1. Install [Lemonade Server](https://github.com/lemonade-sdk/lemonade) following its documentation.
2. Start the server:
   ```bash
   lemonade-server run Flux-2-Klein-4B
   ```
   The server will listen on `http://localhost:8000`. The Flux model will be downloaded automatically on first launch.

> **Note:** The Lemonade Server must be running before you start Lemon Zest. The app will show a "Connecting to server..." overlay until the server is available.

## Installation

### Linux (Snap)

Install from the Snap Store:

```bash
sudo snap install lemon-zest
```

Or build the snap locally:

```bash
sudo snap install snapcraft --classic
snapcraft
sudo snap install lemon-zest_*.snap --dangerous
```

### Linux (From Source)

```bash
git clone https://github.com/phqen1x/lemon-zest.git
cd lemon-zest
npm install
npm start
```

### Windows

#### From Release

Download the latest installer (`.exe`) or portable build from the [Releases](https://github.com/phqen1x/lemon-zest/releases) page.

- **NSIS Installer** — Standard Windows installer with custom install path
- **Portable** — Standalone executable, no installation required

#### From Source

```bash
git clone https://github.com/phqen1x/lemon-zest.git
cd lemon-zest
npm install
npm start
```

## Building Packages

### Linux

```bash
npm install
npx electron-builder --linux dir
```

The unpacked app will be in `dist/linux-unpacked/`.

### Windows

```bash
npm install
npx electron-builder --win
```

This produces both an NSIS installer and a portable executable in `dist/`.

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| R | Rectangle select |
| C | Circle select |
| L | Lasso select |
| B | Brush tool |
| F | Flood fill |
| Ctrl + A | Select all |
| Ctrl + Z | Undo |
| Ctrl + Y | Redo |
| Ctrl + S | Save As |
| Ctrl + = | Zoom in |
| Ctrl + - | Zoom out |
| Ctrl + 0 | Reset zoom |
| Escape | Cancel inpainting |
| F12 | Toggle DevTools |

## License

[MIT](LICENSE)
