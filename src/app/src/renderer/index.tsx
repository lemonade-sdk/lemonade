import { isTauri } from './tauriShim';
import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import { getMobileStoreUrl } from './utils/mobileStoreRedirect';
import '../../assets/favicon.ico';

const userAgent = navigator.userAgent || navigator.vendor || (window as any).opera;
const mobileStoreUrl = getMobileStoreUrl(
  userAgent,
  isTauri(),
  Boolean((window as any).MSStream),
);

if (mobileStoreUrl) {
  window.location.href = mobileStoreUrl;
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
