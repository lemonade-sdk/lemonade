import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import '../../assets/favicon.ico';

// Detect mobile user agents and redirect to appropriate app store
const userAgent = navigator.userAgent || navigator.vendor || (window as any).opera;
if (/android/i.test(userAgent)) {
  window.location.href = 'https://play.google.com/store/apps/details?id=com.lemonade.mobile.chat.ai';
} else if (/iPad|iPhone|iPod/.test(userAgent) && !(window as any).MSStream) {
  window.location.href = 'https://apps.apple.com/ca/app/lemonade-mobile/id6757372210';
} else {
  const root = ReactDOM.createRoot(
    document.getElementById('root') as HTMLElement
  );

  root.render(
    <React.StrictMode>
      <App />
    </React.StrictMode>
  );
}
