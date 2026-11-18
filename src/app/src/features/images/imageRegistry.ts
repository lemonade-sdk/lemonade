/** In-memory image identity for one ChatView session. Never serialize this registry. */
export type ImageOrigin = 'upload' | 'generated' | 'edited';

export interface ConversationImage {
  id: string;
  origin: ImageOrigin;
  original: Blob;
  mimeType: string;
  createdOrder: number;
  messageId?: string;
  sourceImageIds?: string[];
  promptHint?: string;
  analysisInput?: Promise<string>;
  analysisCacheBytes?: number;
}

interface ConversationImages {
  images: Map<string, ConversationImage>;
  order: number;
  disposed: boolean;
  totalBytes: number;
  analysisBytes: number;
}

const MAX_IMAGE_BYTES = 64 * 1024 * 1024;
const MAX_CONVERSATION_BYTES = 512 * 1024 * 1024;
const ANALYSIS_MAX_EDGE = 1024;
const MAX_FALLBACK_IMAGE_BYTES = 8 * 1024 * 1024;
const MAX_ANALYSIS_CACHE_BYTES = 64 * 1024 * 1024;

function validateImage(blob: Blob): void {
  if (!blob.type.toLowerCase().startsWith('image/')) {
    throw new Error(`Unsupported image MIME type: ${blob.type || '(missing)'}.`);
  }
  if (!blob.size || blob.size > MAX_IMAGE_BYTES) {
    throw new Error(`Image must be nonempty and at most ${MAX_IMAGE_BYTES / (1024 * 1024)} MiB.`);
  }
}

function blobDataUrl(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ''));
    reader.onerror = () => reject(reader.error || new Error('Could not read image.'));
    reader.readAsDataURL(blob);
  });
}

async function outputBlob(url: string, signal?: AbortSignal, maxBytes = MAX_IMAGE_BYTES): Promise<Blob> {
  if (!/^(data:image\/|blob:|https?:\/\/)/i.test(url)) {
    throw new Error('Image backend returned an unsupported media URL.');
  }
  const response = await fetch(url, { signal });
  if (!response.ok) throw new Error(`Could not read generated image (HTTP ${response.status}).`);
  const declared = response.headers.get('content-length');
  if (declared && Number(declared) > maxBytes) {
    throw new Error('Generated image exceeds the conversation image memory limit.');
  }
  if (!response.body) throw new Error('Generated image response has no readable stream.');
  const reader = response.body.getReader();
    const chunks: Uint8Array[] = [];
    let received = 0;
    let complete = false;
    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        received += value.byteLength;
        if (received > maxBytes) {
          throw new Error('Generated image exceeds the conversation image memory limit.');
        }
        chunks.push(value);
      }
      complete = true;
    } finally {
      if (!complete) await reader.cancel().catch(() => undefined);
      reader.releaseLock();
    }
  const blob = new Blob(chunks as BlobPart[], {
    type: (response.headers.get('content-type') || '').split(';')[0].trim(),
  });
  validateImage(blob);
  return blob;
}

function fallbackAnalysisInput(original: Blob): Promise<string> {
  if (original.size > MAX_FALLBACK_IMAGE_BYTES) {
    throw new Error('Image is too large for analysis without browser resizing. Use a smaller image.');
  }
  return blobDataUrl(original);
}

async function analysisInput(image: ConversationImage): Promise<string> {
  const original = image.original;
  if (typeof createImageBitmap !== 'function' || typeof document === 'undefined') {
    return fallbackAnalysisInput(original);
  }
  let bitmap: ImageBitmap;
  try {
    bitmap = await createImageBitmap(original);
  } catch {
    return fallbackAnalysisInput(original);
  }
  try {
    const largest = Math.max(bitmap.width, bitmap.height);
    if (largest <= ANALYSIS_MAX_EDGE && original.size <= MAX_FALLBACK_IMAGE_BYTES) {
      return blobDataUrl(original);
    }
    const factor = Math.min(1, ANALYSIS_MAX_EDGE / largest);
    const canvas = document.createElement('canvas');
    canvas.width = Math.max(1, Math.round(bitmap.width * factor));
    canvas.height = Math.max(1, Math.round(bitmap.height * factor));
    const context = canvas.getContext('2d');
    if (!context) return fallbackAnalysisInput(original);
    context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    // Preserve transparency and sharp text by keeping non-JPEG sources lossless.
    const type = original.type === 'image/jpeg' ? 'image/jpeg' : 'image/png';
    const derivative = await new Promise<Blob | null>(resolve => canvas.toBlob(resolve, type, 0.9));
    return derivative ? blobDataUrl(derivative) : fallbackAnalysisInput(original);
  } finally {
    bitmap.close();
  }
}

export class ConversationImageRegistry {
  private readonly conversations = new Map<string, ConversationImages>();
  private readonly closedConversations = new Set<string>();
  // Session-wide counters prevent an ID from one conversation resolving as
  // another conversation's img_u1/img_g1 after a switch or deletion.
  // The random session prefix also prevents stale IDs from an earlier page
  // session being resolved after a refresh and new uploads.
  private readonly sessionKey = Array.from(crypto.getRandomValues(new Uint8Array(6)),
    byte => byte.toString(16).padStart(2, '0')).join('');
  private nextUploadId = 0;
  private nextOutputId = 0;

  private state(id: string): ConversationImages {
    if (this.closedConversations.has(id)) throw new Error('Conversation is closed; image output was discarded.');
    let state = this.conversations.get(id);
    if (!state) {
      state = { images: new Map(), order: 0, disposed: false, totalBytes: 0, analysisBytes: 0 };
      this.conversations.set(id, state);
    }
    return state;
  }

  async validateUploadBytes(files: readonly Blob[]): Promise<void> {
    if (typeof createImageBitmap !== 'function') return;
    for (const file of files) {
      validateImage(file);
      let bitmap: ImageBitmap;
      try {
        bitmap = await createImageBitmap(file);
      } catch {
        throw new Error('The image data could not be decoded. Use a supported image format.');
      }
      bitmap.close();
    }
  }

  addUploads(conversationId: string, files: readonly Blob[], messageId: string): string[] {
    if (!messageId) throw new Error('Image upload needs a message ID.');
    for (const file of files) validateImage(file);
    const state = this.state(conversationId);
    this.ensureCapacity(state, files);
    return files.map(original => {
      state.totalBytes += original.size;
      const id = `img_u${this.sessionKey}_${++this.nextUploadId}`;
      state.images.set(id, {
        id, origin: 'upload', original, mimeType: original.type,
        createdOrder: ++state.order, messageId,
      });
      return id;
    });
  }

  async addOutputs(
    conversationId: string,
    urls: readonly string[],
    origin: 'generated' | 'edited',
    sourceImageIds: readonly string[] = [],
    promptHint = '',
    signal?: AbortSignal,
    messageId?: string,
  ): Promise<string[]> {
    if (signal?.aborted) throw new Error('Image generation was cancelled.');
    if (urls.length === 0) throw new Error('Image backend returned no output.');
    const state = this.state(conversationId);
    if (sourceImageIds.length) this.resolve(conversationId, sourceImageIds);
    const originals: Blob[] = [];
    let remaining = MAX_CONVERSATION_BYTES - state.totalBytes - state.analysisBytes;
    if (remaining <= 0) throw new Error('Conversation image memory limit reached (512 MiB). Start a new conversation or remove an older one.');
    try {
      // A failed batch must not leave some of its image IDs visible.
      for (const url of urls) {
        if (signal?.aborted) throw new Error('Image generation was cancelled.');
        const original = await outputBlob(url, signal, Math.min(MAX_IMAGE_BYTES, remaining));
        originals.push(original);
        remaining -= original.size;
      }
    } catch (error) {
      if (signal?.aborted) throw new Error('Image generation was cancelled.');
      throw error;
    }
    if (signal?.aborted) throw new Error('Image generation was cancelled.');
    if (state.disposed || this.conversations.get(conversationId) !== state) {
      throw new Error('Image output arrived after its conversation was closed.');
    }
    this.ensureCapacity(state, originals);
    return originals.map(original => {
      state.totalBytes += original.size;
      const id = `img_g${this.sessionKey}_${++this.nextOutputId}`;
      state.images.set(id, {
        id, origin, original, mimeType: original.type,
        createdOrder: ++state.order,
        sourceImageIds: [...sourceImageIds], messageId,
        promptHint: promptHint.replace(/\s+/g, ' ').trim().slice(0, 100),
      });
      return id;
    });
  }

  async addOutputsBestEffort(
    conversationId: string,
    urls: readonly string[],
    origin: 'generated' | 'edited',
    sourceImageIds: readonly string[] = [],
    promptHint = '',
    signal?: AbortSignal,
    messageId?: string,
  ): Promise<{ ids?: string[]; warning?: string }> {
    if (urls.length === 0) throw new Error('Image backend returned no output.');
    try {
      return { ids: await this.addOutputs(conversationId, urls, origin, sourceImageIds, promptHint, signal, messageId) };
    } catch (error) {
      if (signal?.aborted) throw error;
      return { warning: error instanceof Error ? error.message : String(error) };
    }
  }

  pruneForRestart(conversationId: string, retainedMessageIds: readonly string[], restartMessageId?: string): void {
    const state = this.conversations.get(conversationId);
    if (!state) return;
    const retained = new Set(retainedMessageIds);
    for (const [id, image] of state.images) {
      // Keep the retried user's original uploads, never the output of the
      // discarded answer. Images from later turns must not remain addressable.
      if (image.messageId && retained.has(image.messageId)
        && (image.origin === 'upload' || image.messageId !== restartMessageId)) continue;
      state.images.delete(id);
      state.totalBytes -= image.original.size;
      state.analysisBytes -= image.analysisCacheBytes || 0;
    }
  }

  private ensureCapacity(state: ConversationImages, additions: readonly Blob[]): void {
    const addedBytes = additions.reduce((total, image) => total + image.size, 0);
    if (state.totalBytes + state.analysisBytes + addedBytes > MAX_CONVERSATION_BYTES) {
      throw new Error('Conversation image memory limit reached (512 MiB). Start a new conversation or remove an older one.');
    }
  }

  resolve(conversationId: string, ids: readonly string[]): ConversationImage[] {
    const state = this.conversations.get(conversationId);
    return ids.map(id => {
      const image = state?.images.get(id);
      if (!image) throw new Error(`Image ${id} is unavailable in this conversation or session. Attach it again if needed.`);
      return image;
    });
  }

  defaultsForMessage(conversationId: string, messageId?: string): string[] {
    const state = this.conversations.get(conversationId);
    if (!state) return [];
    const images = [...state.images.values()];
    const uploads = images.filter(image => image.origin === 'upload' && image.messageId === messageId);
    if (uploads.length > 0) return uploads.map(image => image.id);
    const latest = images[images.length - 1];
    return latest ? [latest.id] : [];
  }

  selectForEdit(conversationId: string, rawIds: unknown, messageId?: string): { ids: string[]; images: Blob[] } {
    const ids = rawIds === undefined ? this.defaultsForMessage(conversationId, messageId) : rawIds;
    if (!Array.isArray(ids) || ids.length === 0 || ids.some(id => typeof id !== 'string' || !/^img_[ug][a-f0-9]{12}_\d+$/.test(id))) {
      throw new Error('image_ids must be a nonempty array of available image IDs.');
    }
    return { ids: [...ids], images: this.resolve(conversationId, ids).map(image => image.original) };
  }

  selectForAnalysis(conversationId: string, rawId: unknown, messageId?: string): string {
    if (rawId !== undefined && (typeof rawId !== 'string' || !/^img_[ug][a-f0-9]{12}_\d+$/.test(rawId))) {
      throw new Error('image_id must be an available image ID.');
    }
    const state = this.conversations.get(conversationId);
    const allImages = [...(state?.images.values() || [])];
    const currentUploads = allImages.filter(image => image.origin === 'upload' && image.messageId === messageId);
    const latest = allImages[allImages.length - 1];
    // After a successful edit/generation in this tool turn, inspect that output
    // rather than silently re-inspecting the user's original upload.
    const newestCurrentOutput = latest && latest.origin !== 'upload'
      && (!currentUploads.length || latest.createdOrder > currentUploads[currentUploads.length - 1].createdOrder);
    const id = rawId === undefined
      ? (newestCurrentOutput ? latest.id : (currentUploads[0]?.id || latest?.id))
      : String(rawId);
    if (!id) throw new Error('No image is available for analysis.');
    this.resolve(conversationId, [id]);
    return id;
  }

  async getAnalysisInput(conversationId: string, rawId: unknown, messageId?: string): Promise<string> {
    const id = this.selectForAnalysis(conversationId, rawId, messageId);
    const image = this.resolve(conversationId, [id])[0];
    if (!image.analysisInput) {
      image.analysisInput = analysisInput(image).then(value => {
        const state = this.conversations.get(conversationId);
        // Conservatively count UTF-16 bytes; keep large conversions transient.
        const bytes = value.length * 2;
        if (state?.images.get(id) === image
          && state.analysisBytes + bytes <= MAX_ANALYSIS_CACHE_BYTES
          && state.totalBytes + state.analysisBytes + bytes <= MAX_CONVERSATION_BYTES) {
          image.analysisCacheBytes = bytes;
          state.analysisBytes += bytes;
        } else {
          image.analysisInput = undefined;
        }
        return value;
      }).catch(error => {
        image.analysisInput = undefined;
        throw error;
      });
    }
    return image.analysisInput;
  }

  listForPlanner(conversationId: string, messageId?: string): string {
    const images = [...(this.conversations.get(conversationId)?.images.values() || [])];
    if (images.length === 0) return '';
    const current = images.filter(image => image.origin === 'upload' && image.messageId === messageId);
    const prior = images.filter(image => !current.includes(image));
    const lines: string[] = ['Available conversation images (IDs are session-only):'];
    if (current.length) {
      lines.push('Current message uploads:');
      current.forEach((image, index) => lines.push(`  image ${index + 1} -> ${image.id}`));
    }
    if (prior.length) {
      const recent = prior.slice(-10).reverse();
      // Keep early originals addressable after a long edit chain, without
      // appending the entire conversation history to each planner request.
      const earliestUploads = prior.filter(image => image.origin === 'upload').slice(0, 6);
      const shown = [...recent, ...earliestUploads.filter(image => !recent.includes(image))];
      const uploadMessages = [...new Set(images.filter(image => image.origin === 'upload').map(image => image.messageId))];
      lines.push('Earlier images (recent results, then earliest originals):');
      for (const image of shown) {
        const source = image.sourceImageIds?.length ? ` from ${image.sourceImageIds.join(',')}` : '';
        const batch = image.origin === 'upload'
          ? `; upload ${uploadMessages.indexOf(image.messageId) + 1}, image ${images.filter(candidate => candidate.messageId === image.messageId).indexOf(image) + 1}`
          : '';
        const hint = image.promptHint ? `; prompt: ${image.promptHint}` : '';
        lines.push(`  ${image.id} (${image.origin}${source}${batch}${hint})`);
      }
      if (prior.length > shown.length) lines.push(`  ${prior.length - shown.length} older image(s) omitted from this index.`);
    }
    return lines.join('\n');
  }

  clearConversation(conversationId: string): void {
    this.closedConversations.add(conversationId);
    const state = this.conversations.get(conversationId);
    if (state) {
      state.disposed = true;
      state.images.clear();
      state.totalBytes = 0;
      state.analysisBytes = 0;
      this.conversations.delete(conversationId);
    }
  }

  clear(): void {
    for (const id of this.conversations.keys()) this.clearConversation(id);
  }
}
