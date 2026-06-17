const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');

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
    filename: 'renderer.bundle.js',
    path: path.resolve(__dirname, 'dist/renderer'),
    clean: true,
  },
  optimization: {
    // GUI3 pulls in large visualization/markdown dependencies. Keep the beta
    // renderer unminified so CI can build it reliably without changing the
    // single-bundle entry expected by Tauri and packaging.
    minimize: false,
  },
  plugins: [
    new HtmlWebpackPlugin({
      template: './src/index.html',
      filename: 'index.html',
    }),
  ],
  devServer: {
    static: path.resolve(__dirname, 'dist/renderer'),
    port: 9123,
    hot: true,
    open: false,
    historyApiFallback: true,
    headers: { 'Cache-Control': 'no-store' },
  },
});
