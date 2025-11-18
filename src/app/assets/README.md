# Assets Directory

Place your application icons here:

- `icon.ico` - Windows icon (256x256 or larger)
- `icon.icns` - macOS icon (512x512 or larger)
- `icon.png` - Linux icon (512x512 or larger)

## Creating Icons

You can use tools like:
- [electron-icon-builder](https://www.npmjs.com/package/electron-icon-builder)
- [iConvert Icons](https://iconverticons.com/online/)
- [CloudConvert](https://cloudconvert.com/)

## Quick Start

If you don't have custom icons yet, the build process will use Electron's default icon.

To generate icons from a single PNG file:

```bash
npm install -g electron-icon-builder
electron-icon-builder --input=./path-to-your-1024x1024-png.png --output=./assets
```

