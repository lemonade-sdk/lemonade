const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const webpack = require('webpack');

module.exports = (env, argv) => {
  const useSystemPackages = process.env.USE_SYSTEM_NODEJS_MODULES === 'true' || process.env.USE_SYSTEM_NODEJS_MODULES === '1';

  const fs = require('fs');

  // When using system packages, check if overlay exists before using it
  let katexAlias = {};
  let useSystemKatexCss = false;
  if (useSystemPackages) {
    const overlayPath = path.resolve(__dirname, 'katex-overlay/katex/index.js');
    const overlayCssPath = path.resolve(__dirname, 'katex-overlay/katex/dist/katex.min.css');
    const systemKatexFontsPath = '/usr/share/fonts/truetype/katex';
    // Only use overlay if both the shim AND the CSS file exist
    if (fs.existsSync(overlayPath) && fs.existsSync(overlayCssPath)) {
      katexAlias = {
        'katex$': overlayPath,
        'katex/dist/katex.min.css': overlayCssPath,
      };

      // Only disable URL rewriting when system fonts are also present
      if (fs.existsSync(systemKatexFontsPath)) {
        useSystemKatexCss = true;
      }
    }
  }

  // Resolve polyfills conditionally - try to resolve them, but fall back to false if not available
  let bufferPolyfill = false;
  let processPolyfill = false;
  try {
    bufferPolyfill = require.resolve('buffer/');
  } catch (e) {
    // buffer package not installed locally
  }
  try {
    processPolyfill = require.resolve('process/browser');
  } catch (e) {
    // process package not installed locally
  }

  // The shared renderer source lives in ../app/src (sibling tree). The
  // web-app build resolves it directly via these relative paths instead of
  // checking in OS-level symlinks (which break Windows checkouts unless
  // core.symlinks=true and developer mode are both enabled). The CMake
  // staging step (BuildWebApp.cmake) preserves this layout by copying both
  // src/app and src/web-app into the build directory side by side.
  const config = {
    mode: argv.mode || 'development',
    entry: '../app/src/renderer/index.tsx',
    target: 'web',  // Changed from 'electron-renderer' to 'web' for browser
    devtool: argv.mode === 'production' ? false : 'source-map',
    module: {
      rules: [
        {
          test: /\.tsx?$/,
          use: {
            loader: 'ts-loader',
            options: {
              transpileOnly: true,  // Skip type checking for faster builds
            }
          },
          exclude: /node_modules/,
        },
        {
          test: /\.css$/,
          use: ['style-loader', 'css-loader'],
          exclude: useSystemPackages ? /katex\.min\.css$/ : undefined,
        },
        {
          test: /\.svg$/,
          type: 'asset/resource',
        },
        {
          test: /\.(png|jpg|jpeg|gif|ico|woff|woff2|ttf|eot)$/,
          type: 'asset/resource',
          generator: {
            filename: '[name][ext]'
          }
        },
        {
          test: /\.json$/,
          type: 'json',
        },
      ],
    },
    resolve: {
      extensions: ['.tsx', '.ts', '.js', '.jsx'],
      modules: useSystemPackages ? [
        path.resolve(__dirname, 'node_modules'),  // Local node_modules for build tools
        '/usr/share/nodejs',  // System packages for runtime dependencies
        '/usr/lib/nodejs',
        '/usr/share/javascript',
        'node_modules'  // fallback to standard resolution
      ] : [
        path.resolve(__dirname, 'node_modules'),
        'node_modules'
      ],
      alias: {
        ...katexAlias,
        // The shared renderer (symlinked from ../app/src) imports @tauri-apps/*
        // modules in tauriShim.ts. The web-app intentionally excludes those
        // packages; alias each specifier to a no-op stub. The shim never calls
        // into them at runtime in pure-web mode (isTauri() is always false).
        '@tauri-apps/api/core$': path.resolve(__dirname, 'tauri-stub.js'),
        '@tauri-apps/api/event$': path.resolve(__dirname, 'tauri-stub.js'),
        '@tauri-apps/api/window$': path.resolve(__dirname, 'tauri-stub.js'),
        '@tauri-apps/plugin-opener$': path.resolve(__dirname, 'tauri-stub.js'),
        '@tauri-apps/plugin-clipboard-manager$': path.resolve(__dirname, 'tauri-stub.js'),
      },
      fallback: {
        "path": false,
        "fs": false,
        "url": false,
        "util": false,
        "stream": false,
        "http": false,
        "https": false,
        "buffer": bufferPolyfill,
        "process": processPolyfill,
      },
    },
    output: {
      filename: 'renderer.bundle.js',
      path: process.env.WEBPACK_OUTPUT_PATH || path.resolve(__dirname, 'dist/renderer'),
    },
    plugins: [
      new HtmlWebpackPlugin({
        template: '../app/src/renderer/index.html',
        filename: 'index.html',
      }),
      ...(bufferPolyfill && processPolyfill ? [
        new webpack.ProvidePlugin({
          process: 'process/browser',
          Buffer: ['buffer', 'Buffer'],
        }),
      ] : []),
    ],
  };

  // Special handling for system packages (Debian)
  if (useSystemKatexCss) {
    // Handle KaTeX CSS without URL resolution (fonts are provided by the system/overlay)
    config.module.rules.unshift({
      test: /katex\.min\.css$/,
      use: ['style-loader', { loader: 'css-loader', options: { url: false } }],
    });
  }

  return config;
};
