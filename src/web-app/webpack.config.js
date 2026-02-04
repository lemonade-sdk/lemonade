const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');

module.exports = (env, argv) => ({
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
    modules: [
      path.resolve(__dirname, 'node_modules'),  // web-app/node_modules
      'node_modules'  // fallback to standard resolution
    ],
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
    path: process.env.WEBPACK_OUTPUT_PATH || path.resolve(__dirname, 'dist/renderer'),
  },
  plugins: [
    new HtmlWebpackPlugin({
      template: './src/renderer/index.html',
      filename: 'index.html',
    }),
  ],
});
