/** Routes analyze_image back to the current vision chat model without a second inference. */
import type { ChatMessage } from '../../api';
import type { ToolExecutionPayload } from '../../hooks/useChatStreaming';
import type { ConversationImageRegistry } from './imageRegistry';

// Bound simultaneous image conversions, not the number of images the user may inspect.
const MAX_CONCURRENT_IMAGE_RESOLUTIONS = 4;

export interface DirectImageAnalysisContext {
  imageRegistry?: ConversationImageRegistry;
  conversationId?: string;
  messageId?: string;
  directInputImageIds?: readonly string[];
}

/** Return only metadata to the planner; the image itself is delivered in the next chat request. */
export async function prepareDirectImageAnalysis(
  callId: string,
  question: string,
  rawImageId: unknown,
  context: DirectImageAnalysisContext,
  signal?: AbortSignal,
): Promise<ToolExecutionPayload> {
  if (!context.imageRegistry || !context.conversationId) {
    throw new Error('Conversation image registry is unavailable.');
  }
  const imageId = context.imageRegistry.selectForAnalysis(context.conversationId, rawImageId, context.messageId);
  const alreadyVisible = context.directInputImageIds?.includes(imageId) === true;
  // Validate and cache the analysis representation before reporting success.
  if (!alreadyVisible) await context.imageRegistry.getAnalysisInput(context.conversationId, imageId);
  if (signal?.aborted) throw new Error('Image inspection was cancelled.');
  return {
    role: 'tool',
    tool_call_id: callId,
    content: JSON.stringify({
      status: alreadyVisible ? 'already_visible' : 'attached_to_next_chat_request',
      image_id: imageId,
      question,
      instruction: 'Inspect this image directly and answer the question. Do not call analyze_image again for the same image.',
    }),
    displayResult: alreadyVisible ? 'Image already visible to chat model' : 'Image provided to chat model',
    ...(!alreadyVisible ? { attachImageIds: [imageId] } : {}),
  };
}

/** Direct analysis attaches image parts to the next tool follow-up, not to the tool result. */
export async function directImageFollowupContent(
  guidance: string,
  results: readonly ToolExecutionPayload[],
  resolveImage?: (imageId: string) => Promise<string>,
  signal?: AbortSignal,
): Promise<ChatMessage['content']> {
  const ids = [...new Set(results.filter(result => !result.error).flatMap(result => result.attachImageIds || []))];
  if (ids.length === 0) return guidance;
  const resolve = resolveImage;
  if (!resolve) throw new Error('Chat image attachment resolver is unavailable.');

  // More than four images can be requested in the same tool round (e.g. seven
  // generated images followed by seven analyze_image calls). Preserve all IDs;
  // the backend, not an arbitrary GUI limit, determines what its VLM can accept.
  // Pair every image with its ID so deduplication never obscures which result
  // the model is inspecting.
  const parts: Exclude<ChatMessage['content'], string | null> = [{
    type: 'text',
    text: `${guidance}\nAll ${ids.length} requested image(s) follow with their IDs. Inspect each image, answer the tool questions, and do not call analyze_image again for these images.`,
  }];
  for (let offset = 0; offset < ids.length; offset += MAX_CONCURRENT_IMAGE_RESOLUTIONS) {
    if (signal?.aborted) throw new Error('Image inspection was cancelled.');
    const batchIds = ids.slice(offset, offset + MAX_CONCURRENT_IMAGE_RESOLUTIONS);
    const urls = await Promise.all(batchIds.map(id => resolve(id)));
    if (signal?.aborted) throw new Error('Image inspection was cancelled.');
    for (let index = 0; index < batchIds.length; index++) {
      if (!urls[index].startsWith('data:image/')) {
        throw new Error(`Image ${batchIds[index]} could not be prepared for inspection.`);
      }
      parts.push(
        { type: 'text', text: `Image ${offset + index + 1} (${batchIds[index]}):` },
        { type: 'image_url', image_url: { url: urls[index] } },
      );
    }
  }
  return parts;
}
