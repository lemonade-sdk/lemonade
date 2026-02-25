export interface MarketplaceApp {
  id: string;
  name: string;
  description?: string;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: {
    app?: string;
    guide?: string;
    video?: string;
  };
}

export interface MarketplaceCategory {
  id: string;
  label: string;
}

export const APPS_JSON_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';
