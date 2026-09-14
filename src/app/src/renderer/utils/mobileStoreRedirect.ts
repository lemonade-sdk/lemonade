const ANDROID_STORE_URL =
  'https://play.google.com/store/apps/details?id=com.lemonade.mobile.chat.ai';
const IOS_STORE_URL = 'https://apps.apple.com/ca/app/lemonade-mobile/id6757372210';

export function getMobileStoreUrl(
  userAgent: string,
  isDesktopApp: boolean,
  hasMsStream: boolean,
): string | null {
  if (isDesktopApp) {
    return null;
  }

  if (/android/i.test(userAgent)) {
    return ANDROID_STORE_URL;
  }

  if (/iPad|iPhone|iPod/.test(userAgent) && !hasMsStream) {
    return IOS_STORE_URL;
  }

  return null;
}
