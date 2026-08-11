const fs = require('fs');
const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const webpack = require('webpack');

function firstExisting(paths) {
  return paths.find(candidate => fs.existsSync(candidate)) || null;
}

class EmitAppCssPlugin {
  constructor(rendererRoot, appRoot) {
    this.rendererRoot = rendererRoot;
    this.appRoot = appRoot;
  }

  apply(compiler) {
    compiler.hooks.thisCompilation.tap('EmitAppCssPlugin', compilation => {
      const cssFiles = [
        path.resolve(this.rendererRoot, 'styles/tokens.css'),
        path.resolve(this.rendererRoot, 'styles/styles.css'),
      ].filter(file => fs.existsSync(file));
      const logoFile = firstExisting([
        path.resolve(this.appRoot, 'assets/logo.svg'),
        path.resolve(this.rendererRoot, '../assets/logo.svg'),
      ]);

      for (const file of cssFiles) compilation.fileDependencies.add(file);
      if (logoFile) compilation.fileDependencies.add(logoFile);

      compilation.hooks.processAssets.tap(
        {
          name: 'EmitAppCssPlugin',
          stage: webpack.Compilation.PROCESS_ASSETS_STAGE_ADDITIONAL,
        },
        () => {
          if (cssFiles.length) {
            const css = cssFiles
              .map(file => fs.readFileSync(file, 'utf8'))
              .join('\n')
              .replaceAll('../../assets/logo.svg', 'logo.svg')
              + '\n:root{--lemonade-full-styles-loaded:1;}\n';
            compilation.emitAsset('app.css', new webpack.sources.RawSource(css));
          }
          if (logoFile) {
            compilation.emitAsset('logo.svg', new webpack.sources.RawSource(fs.readFileSync(logoFile)));
          }
        },
      );
    });
  }
}

module.exports = (env, argv) => {
  // The CMake web build stages app/ and web-app/ next to each other. GUI3 used
  // both a flattened app/src/index.tsx layout and the newer app/src/renderer/
  // layout during development; support either so this config is drop-in safe.
  const appRoot = path.resolve(__dirname, '../app');
  const rendererRoot = firstExisting([
    path.resolve(appRoot, 'src/index.tsx'),
    path.resolve(appRoot, 'src/renderer/index.tsx'),
  ]);
  if (!rendererRoot) {
    throw new Error('Cannot find shared Lemonade renderer entry under ../app/src');
  }
  const sourceRoot = path.dirname(rendererRoot);
  const entryFile = rendererRoot;
  const templateFile = path.resolve(sourceRoot, 'index.html');
  const isProduction = (argv.mode || 'production') === 'production';
  const useSystemPackages = process.env.USE_SYSTEM_NODEJS_MODULES === 'true'
    || process.env.USE_SYSTEM_NODEJS_MODULES === '1';

  let katexAlias = {};
  let useSystemKatexCss = false;
  if (useSystemPackages) {
    const overlayPath = path.resolve(__dirname, 'katex-overlay/katex/index.js');
    const overlayCssPath = path.resolve(__dirname, 'katex-overlay/katex/dist/katex.min.css');
    const systemKatexFontsPath = '/usr/share/fonts/truetype/katex';
    if (fs.existsSync(overlayPath) && fs.existsSync(overlayCssPath)) {
      katexAlias = {
        'katex$': overlayPath,
        'katex/dist/katex.min.css': overlayCssPath,
      };
      if (fs.existsSync(systemKatexFontsPath)) useSystemKatexCss = true;
    }
  }

  const systemPackageAliases = useSystemPackages ? {
    'dompurify$': path.resolve(__dirname, 'system-stubs/dompurify.ts'),
    'mermaid$': path.resolve(__dirname, 'system-stubs/mermaid.ts'),
    'recharts$': path.resolve(__dirname, 'system-stubs/recharts.tsx'),
    'highlight.js/styles/github-dark.css$': path.resolve(__dirname, 'system-stubs/highlight-github-dark.css'),
  } : {};

  let bufferPolyfill = false;
  let processPolyfill = false;
  try { bufferPolyfill = require.resolve('buffer/'); } catch {}
  try { processPolyfill = require.resolve('process/browser'); } catch {}

  const tauriStub = path.resolve(__dirname, 'tauri-stub.js');
  const normalize = value => value.replace(/\\/g, '/');
  const sourceRootNormalized = `${normalize(sourceRoot)}/`;
  const shellPatterns = [
    /\/App\.tsx$/,
    /\/components\/ShellIcon\.tsx$/,
    /\/startupScheduler\.ts$/,
    /\/features\/navigation\//,
    /\/features\/models\/modelState\.ts$/,
    /\/features\/apps\/marketplace\.ts$/,
  ];

  const isReactSource = module => {
    const resource = module && module.resource ? normalize(module.resource) : '';
    return /(?:^|\/)(?:node_modules|usr\/share\/nodejs|usr\/lib\/nodejs)\/(react|react-dom|scheduler)(?:\/|$)/.test(resource);
  };

  const isRendererSource = module => {
    const resource = module && module.resource ? normalize(module.resource) : '';
    return resource.startsWith(sourceRootNormalized);
  };
  const isShellSource = module => {
    if (!isRendererSource(module)) return false;
    const resource = normalize(module.resource);
    return shellPatterns.some(pattern => pattern.test(resource));
  };
  const isStartupAppSource = module => {
    if (!isRendererSource(module)) return false;
    const resource = normalize(module.resource);
    if (resource === normalize(entryFile)) return false;
    return !isShellSource(module);
  };

  const config = {
    // Production is the safe default for the server-delivered UI. Developers
    // can still request --mode development explicitly.
    mode: argv.mode || 'production',
    entry: {
      renderer: entryFile,
    },
    target: 'web',
    devtool: isProduction ? false : 'source-map',

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
          exclude: useSystemPackages ? /katex\.min\.css$/ : undefined,
        },
        { test: /\.svg$/, type: 'asset/resource' },
        {
          test: /\.(png|jpg|jpeg|gif|ico|woff|woff2|ttf|eot)$/,
          type: 'asset/resource',
          generator: { filename: '[name][ext]' },
        },
        { test: /\.json$/, type: 'json' },
      ],
    },

    resolve: {
      extensions: ['.tsx', '.ts', '.js', '.jsx'],
      modules: useSystemPackages
        ? [
            path.resolve(__dirname, 'node_modules'),
            path.resolve(appRoot, 'node_modules'),
            '/usr/share/nodejs',
            '/usr/lib/nodejs',
            '/usr/share/javascript',
            'node_modules',
          ]
        : [
            path.resolve(__dirname, 'node_modules'),
            path.resolve(appRoot, 'node_modules'),
            'node_modules',
          ],
      alias: {
        ...katexAlias,
        ...systemPackageAliases,
        '@tauri-apps/api/core$': tauriStub,
        '@tauri-apps/api/event$': tauriStub,
        '@tauri-apps/api/window$': tauriStub,
        '@tauri-apps/plugin-opener$': tauriStub,
        '@tauri-apps/plugin-clipboard-manager$': tauriStub,
      },
      fallback: {
        path: false,
        fs: false,
        url: false,
        util: false,
        stream: false,
        http: false,
        https: false,
        buffer: bufferPolyfill,
        process: processPolyfill,
      },
    },

    output: {
      filename: '[name].bundle.js',
      chunkFilename: isProduction ? '[name].[contenthash:8].chunk.js' : '[name].chunk.js',
      path: process.env.WEBPACK_OUTPUT_PATH || path.resolve(__dirname, 'dist/renderer'),
      publicPath: 'auto',
      clean: true,
    },

    optimization: {
      minimize: isProduction,
      usedExports: true,
      sideEffects: true,
      concatenateModules: isProduction,
      moduleIds: isProduction ? 'deterministic' : 'named',
      chunkIds: isProduction ? 'deterministic' : 'named',

      // lemond currently serves a large response much more slowly than the
      // browser can parse/evaluate it. Keep the bootstrap tiny and turn the
      // default Chat path into a handful of *initial* chunks. HtmlWebpackPlugin
      // inserts every initial script into HTML, so the browser fetches these in
      // parallel rather than waiting for React.lazy() to discover the next one.
      runtimeChunk: 'single',
      splitChunks: {
        chunks: 'all',
        maxInitialRequests: 6,
        maxAsyncRequests: 20,
        cacheGroups: {
          react: {
            test: isReactSource,
            name: 'react-core',
            chunks: 'all',
            priority: 100,
            enforce: true,
          },
          startupShell: {
            test: isShellSource,
            name: 'startup-shell',
            chunks: 'initial',
            priority: 80,
            enforce: true,
          },
          startupApp: {
            test: isStartupAppSource,
            name: 'startup-chat',
            chunks: 'initial',
            priority: 70,
            enforce: true,
          },
          mermaid: {
            test: /[\\/]node_modules[\\/](mermaid|@mermaid-js|cytoscape|dagre-d3-es)[\\/]/,
            name: 'mermaid',
            chunks: 'async',
            priority: 60,
            enforce: true,
          },
          charts: {
            test: /[\\/]node_modules[\\/](recharts|victory-vendor|d3|d3-.*)[\\/]/,
            name: 'charts',
            chunks: 'async',
            priority: 55,
            enforce: true,
          },
          markdown: {
            test: /[\\/]node_modules[\\/](highlight\.js|markdown-it|katex|markdown-it-texmath|dompurify)[\\/]/,
            name: 'markdown',
            chunks: 'async',
            priority: 55,
            enforce: true,
          },
          asyncVendors: {
            test: /[\\/]node_modules[\\/]/,
            chunks: 'async',
            name: false,
            priority: 0,
            reuseExistingChunk: true,
          },
          default: false,
          defaultVendors: false,
        },
      },
    },

    plugins: [
      new EmitAppCssPlugin(sourceRoot, appRoot),
      new webpack.DefinePlugin({
        'process.env.LEMONADE_BASE_URL': JSON.stringify(process.env.LEMONADE_BASE_URL || ''),
      }),
      new HtmlWebpackPlugin({
        filename: 'index.html',
        scriptLoading: 'defer',
        templateContent: () => {
          const template = fs.readFileSync(templateFile, 'utf8');
          const criticalCssFile = path.resolve(sourceRoot, 'styles/critical.generated.css');
          const criticalCss = fs.existsSync(criticalCssFile) ? fs.readFileSync(criticalCssFile, 'utf8') : '';
          return template.replace('/*__LEMONADE_CRITICAL_CSS__*/', criticalCss);
        },
      }),
      ...(bufferPolyfill && processPolyfill ? [
        new webpack.ProvidePlugin({
          process: 'process/browser',
          Buffer: ['buffer', 'Buffer'],
        }),
      ] : []),
    ],

    performance: { hints: false },
  };

  if (useSystemKatexCss) {
    config.module.rules.unshift({
      test: /katex\.min\.css$/,
      use: ['style-loader', { loader: 'css-loader', options: { url: false } }],
    });
  }

  return config;
};
