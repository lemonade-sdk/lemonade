const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const webpack = require('webpack');

module.exports = (env, argv) => ({
  mode: argv.mode || 'development',

  // Keep the expected renderer.bundle.js entry name while allowing webpack
  // to emit additional vendor, chart, and markdown chunks.
  entry: {
    renderer: './src/index.tsx',
  },

  target: 'web',
  devtool: argv.mode === 'production' ? false : 'source-map',

  module: {
    rules: [
      {
        test: /\.tsx?$/,
        use: {
          loader: 'ts-loader',
          options: {
            transpileOnly: true,
          },
        },
        exclude: /node_modules/,
      },
      {
        test: /\.css$/,
        use: ['style-loader', 'css-loader'],
      },
    ],
  },

  resolve: {
    extensions: ['.tsx', '.ts', '.js', '.jsx'],
  },

  output: {
    filename: '[name].bundle.js',
    chunkFilename: '[name].chunk.js',

    // GUI3/Tauri packaging expects the renderer application here.
    path: path.resolve(__dirname, 'dist/renderer'),

    // Resolve lazy-loaded chunks relative to the actual renderer bundle.
    // This also works when the frontend is hosted below a sub-path.
    publicPath: 'auto',
    clean: true,
  },

  optimization: {
    // GUI3 contains large visualization and markdown dependencies.
    // Avoid expensive minification in CI and development builds.
    minimize: false,

    splitChunks: {
      chunks: 'all',
      cacheGroups: {
        charts: {
          test: /[\\/]node_modules[\\/](recharts|d3-.*|victory-vendor)[\\/]/,
          name: 'charts',
          priority: 20,
        },
        markdown: {
          test: /[\\/]node_modules[\\/](highlight\.js|markdown-it|katex|markdown-it-texmath)[\\/]/,
          name: 'markdown',
          priority: 20,
        },
        vendors: {
          test: /[\\/]node_modules[\\/]/,
          name: 'vendors',
          priority: 10,
        },
      },
    },
  },

  // @google/model-viewer contains an optional expression-based import.
  // Suppress only this known warning.
  ignoreWarnings: [
    warning =>
      /Critical dependency: the request of a dependency is an expression/.test(
        warning.message || '',
      ) &&
      /model-viewer\.min\.js/.test(
        warning.module?.resource || warning.moduleName || '',
      ),
  ],

  plugins: [
    new HtmlWebpackPlugin({
      template: './src/index.html',
      filename: 'index.html',
    }),

    new webpack.DefinePlugin({
      'process.env.LEMONADE_BASE_URL': JSON.stringify(
        process.env.LEMONADE_BASE_URL || '',
      ),
    }),
  ],

  devServer: {
    static: path.resolve(__dirname, 'dist/renderer'),
    port: 9123,
    hot: true,
    open: false,
    historyApiFallback: true,
    allowedHosts: 'all',

    headers: {
      'Cache-Control': 'no-store',
    },

    client: {
      overlay: {
        errors: true,
        warnings: false,
      },
    },
  },

  performance: {
    hints: false,
  },
});