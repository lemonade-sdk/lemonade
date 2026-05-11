// Single source of truth for the fixed image size used by collection-mode
// image tools. Drives:
//   - the `size` field on /images/generations and /images/edits requests
//   - the inline `height` on rendered collection images
//   - the `{image_size}` placeholder in toolDefinitions.json descriptions
//
// 2:1 aspect ratio with 64-aligned dimensions for compatibility across
// SD/SDXL/Flux variants. See SDServer::resolve_size for the server-side
// passthrough.

export const COLLECTION_IMAGE_SIZE = '512x256';

const [w, h] = COLLECTION_IMAGE_SIZE.split('x').map(Number);
export const COLLECTION_IMAGE_WIDTH = w;
export const COLLECTION_IMAGE_HEIGHT = h;
