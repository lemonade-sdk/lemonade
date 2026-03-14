const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const webpack = require('webpack');

module.exports = (env, argv) => {
  const useSystemPackages = process.env.USE_SYSTEM_NODEJS_MODULES === 'true' || process.env.USE_SYSTEM_NODEJS_MODULES === '1';

  const fs = require('fs');

  // When using system packages, check if overlay exists before using it
  let katexAlias = {};
  if (useSystemPackages) {
    const overlayPath = path.resolve(__dirname, 'katex-overlay/katex/index.js');
    const overlayCssPath = path.resolve(__dirname, 'katex-overlay/katex/dist/katex.min.css');
    // Only use overlay if both the shim AND the CSS file exist
    if (fs.existsSync(overlayPath) && fs.existsSync(overlayCssPath)) {
      katexAlias = {
        'katex$': overlayPath,
        'katex/dist/katex.min.css': overlayCssPath,
      };
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

  const config = {
    mode: argv.mode || 'development',
    entry: './src/renderer/index.tsx',
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
      alias: katexAlias,
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
        template: './src/renderer/index.html',
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
  if (useSystemPackages) {
    // Handle KaTeX CSS without URL resolution (fonts are symlinked separately)
    config.module.rules.unshift({
      test: /katex\.min\.css$/,
      use: ['style-loader', { loader: 'css-loader', options: { url: false } }],
    });

    // Handle favicon
    config.module.rules.push({ test: /\.ico$/, type: 'asset/resource' });
  }

  return config;
};
