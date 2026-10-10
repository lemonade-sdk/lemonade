// The chat model override pins a conversation's own model ahead of the app-level
// selection, so an old thread keeps answering on the model it started with even
// when that model is no longer resident.
//
// It is conversation-scoped state: it must never outlive the conversation that set
// it. An override that survives into a new chat outranks a model that is loaded
// right now, so the next prompt silently reloads a model the user already walked
// away from.

export interface ConversationModelState {
  model?: string;
  currentModel: string;
  loaded: boolean;
}

export interface ChatModelOverrideTransition {
  override: string | null;
  select: string | null;
}

export function activateConversationModel(
  override: string | null,
  { model, currentModel, loaded }: ConversationModelState,
): ChatModelOverrideTransition {
  if (!model || model === currentModel) return { override, select: null };
  // Resident models go through the real selection; the override exists only to
  // display a model the send path has yet to load.
  return loaded ? { override: null, select: model } : { override: model, select: null };
}

export function releaseConversationModel(override: string | null, wasActive: boolean): string | null {
  return wasActive ? null : override;
}
