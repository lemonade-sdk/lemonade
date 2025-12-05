const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');

module.exports = (env, argv) => ({
  mode: argv.mode || 'development',
  entry: './src/renderer/index.tsx',
  target: 'electron-renderer',
  devtool: argv.mode === 'production' ? false : 'source-map',
  module: {
    rules: [
      {
        test: /\.tsx?$/,
        use: 'ts-loader',
        exclude: /node_modules/,
      },
      {
        test: /\.css$/,
        use: ['style-loader', 'css-loader'],
      },
      {
        test: /\.svg$/,
        type: 'asset/resource',
      },
      {
        test: /\.json$/,
        type: 'json',
      },
    ],
  },
  resolve: {
    extensions: ['.tsx', '.ts', '.js', '.jsx'],
    fallback: {
      "path": false,
      "fs": false,
      "url": false,
      "util": false,
      "stream": false,
      "buffer": false,
      "process": false,
    },
  },
  output: {
    filename: 'renderer.bundle.js',
    path: path.resolve(__dirname, 'dist/renderer'),
  },
  plugins: [
    new HtmlWebpackPlugin({
      template: './src/renderer/index.html',
      filename: 'index.html',
    }),
  ],
});
