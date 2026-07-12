const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const webpack = require('webpack');

module.exports = (env, argv) => ({
  mode: argv.mode || 'development',
  entry: './src/index.tsx',
  target: 'web',
  devtool: argv.mode === 'production' ? false : 'source-map',
  module: {
    rules: [
      {
        test: /\.tsx?$/,
        use: {
          loader: 'ts-loader',
          options: { transpileOnly: true },
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
    path: path.resolve(__dirname, 'dist'),
    // Derive async chunk URLs from the actual main bundle URL. This works at
    // both / and /web-app/ and avoids document-relative chunk resolution.
    publicPath: 'auto',
    clean: true,
  },
  optimization: {
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
  // @google/model-viewer contains one optional expression-based import. It is
  // valid in the browser and already used by GUI2; suppress only that known
  // parser warning so webpack-dev-server never covers the app with an overlay.
  ignoreWarnings: [
    warning => /Critical dependency: the request of a dependency is an expression/.test(warning.message || '')
      && /model-viewer\.min\.js/.test(warning.module?.resource || warning.moduleName || ''),
  ],
  plugins: [
    new HtmlWebpackPlugin({
      template: './src/index.html',
      filename: 'index.html',
    }),
    new webpack.DefinePlugin({
      'process.env.LEMONADE_BASE_URL': JSON.stringify(process.env.LEMONADE_BASE_URL || ''),
    }),
  ],
  devServer: {
    static: path.resolve(__dirname, 'dist'),
    port: 8080,
    hot: true,
    open: false,
    historyApiFallback: true,
    allowedHosts: 'all',
    headers: { 'Cache-Control': 'no-store' },
    client: {
      overlay: { errors: true, warnings: false },
    },
  },
  performance: {
    hints: false,
  },
});
