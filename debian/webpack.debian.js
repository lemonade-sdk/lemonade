const path = require('path');
const webpack = require('webpack');
const base = require('../src/web-app/webpack.config.js');

module.exports = (env, argv) => {
  const cfg = typeof base === 'function' ? base(env, argv) : base;
  cfg.module = cfg.module || {};
  cfg.module.rules = cfg.module.rules || [];

  // Avoid KaTeX font emission from CSS
  cfg.module.rules.unshift({
    test: /katex\.min\.css$/,
    use: ['style-loader', { loader: 'css-loader', options: { url: false } }],
  });

  // Ensure the generic CSS rule doesn't also process KaTeX CSS
  for (const rule of cfg.module.rules) {
    if (rule && rule.test && rule.test.toString() === /\.css$/.toString()) {
      rule.exclude = /katex\.min\.css$/;
    }
  }

  // Ensure favicon is handled
  cfg.module.rules.push({ test: /\.ico$/, type: 'asset/resource' });

  cfg.resolve = cfg.resolve || {};
  cfg.resolve.alias = cfg.resolve.alias || {};

  // Resolve katex to system JS and CSS (copied into build dir)
  cfg.resolve.alias['katex$'] = path.resolve(__dirname, 'web-app-overlay/katex/index.js');
  cfg.resolve.alias['katex/dist/katex.min.css'] = path.resolve(__dirname, 'web-app-overlay/katex/dist/katex.min.css');

  // Webpack 5 doesn't include Node.js polyfills by default
  // For browser builds, use empty modules for Node.js core modules
  cfg.resolve.fallback = cfg.resolve.fallback || {};
  cfg.resolve.fallback.http = false;
  cfg.resolve.fallback.https = false;
  cfg.resolve.fallback.buffer = require.resolve('buffer/');
  cfg.resolve.fallback.process = require.resolve('process/browser');

  cfg.plugins = cfg.plugins || [];
  cfg.plugins.unshift(
    new webpack.ProvidePlugin({
      process: 'process/browser',
      Buffer: ['buffer', 'Buffer'],
    })
  );

  return cfg;
};
