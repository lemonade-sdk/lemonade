// Fixed image size used by Omni collection image tools.
//
// Main/GUI2 sources this value from toolDefinitions.json. GUI3 keeps the
// default tool definitions in omniTools.ts for now, so this tiny config file is
// the shared local source of truth for both the Omni runtime and the API client.
// Keep this value aligned with main's toolDefinitions.image_size until merging makes it superfluous. 
export const COLLECTION_IMAGE_SIZE = '512x256';

const [collectionImageWidth, collectionImageHeight] = COLLECTION_IMAGE_SIZE.split('x').map(Number);
export const COLLECTION_IMAGE_WIDTH = collectionImageWidth;
export const COLLECTION_IMAGE_HEIGHT = collectionImageHeight;
