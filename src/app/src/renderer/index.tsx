import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';

// Signal that bundle loaded
console.log('🚀 renderer/index.tsx executing!');
if ((window as any).__LEMONADE_BUNDLE_LOADED__) {
  (window as any).__LEMONADE_BUNDLE_LOADED__();
}

console.log('React:', React);
console.log('ReactDOM:', ReactDOM);
console.log('App:', App);

const rootElement = document.getElementById('root');
console.log('Root element found:', rootElement);

if (!rootElement) {
  console.error('❌ FATAL: Root element not found!');
  throw new Error('Root element #root not found in DOM');
}

try {
  console.log('Creating React root...');
  const root = ReactDOM.createRoot(rootElement as HTMLElement);
  
  console.log('Rendering App...');
  root.render(
    <React.StrictMode>
      <App />
    </React.StrictMode>
  );
  console.log('✅ React render called successfully!');
} catch (error) {
  console.error('❌ FATAL ERROR during React initialization:', error);
  throw error;
}

