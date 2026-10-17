const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

(async () => {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-mcp-runtime-'));
  try {
    await new Promise((resolve, reject) => webpack({
      mode: 'development',
      target: 'node',
      entry: path.resolve(__dirname, 'fixtures/mcp-runtime-entry.ts'),
      output: { path: outputPath, filename: 'test.cjs' },
      resolve: { extensions: ['.ts', '.tsx', '.js'] },
      module: { rules: [{ test: /\.tsx?$/, use: { loader: 'ts-loader', options: { transpileOnly: true, compilerOptions: { rootDir: '.' } } }, exclude: /node_modules/ }] },
      optimization: { minimize: false },
    }, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    }));
    const bundled = require(path.join(outputPath, 'test.cjs'));
    await bundled.default;
  } finally {
    setTimeout(() => fs.rmSync(outputPath, { recursive: true, force: true }), 100);
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
