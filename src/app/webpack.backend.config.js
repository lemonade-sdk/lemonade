const path = require('path');

module.exports = {
  mode: 'production',
  entry: './src/backend/server.ts',
  target: 'node',
  devtool: 'source-map',
  module: {
    rules: [
      {
        test: /\.tsx?$/,
        use: 'ts-loader',
        exclude: /node_modules/,
      },
    ],
  },
  resolve: {
    extensions: ['.tsx', '.ts', '.js'],
  },
  output: {
    filename: 'server.js',
    path: path.resolve(__dirname, 'dist/backend'),
  },
  externals: {
    // Don't bundle these - they're provided by Electron/Node
  },
  optimization: {
    minimize: false, // Keep readable for debugging
  },
};

