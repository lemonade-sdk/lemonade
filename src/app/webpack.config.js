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
    filename: '[name].bundle.js',
    path: path.resolve(__dirname, 'dist'),
    clean: true,
  },
  optimization: {
    // Prototype build: keep production output deterministic and fast. The full
    // dependency graph is large enough that default Terser minification can
    // stall local verification, while the UI is still under active review.
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
  plugins: [
    new HtmlWebpackPlugin({
      template: './src/index.html',
      filename: 'index.html',
    }),
  ],
  devServer: {
    static: path.resolve(__dirname, 'dist'),
    port: 8080,
    hot: true,
    open: false,
    historyApiFallback: true,
  },
});
