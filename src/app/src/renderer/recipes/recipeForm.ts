import { BackendOption, BackendRecipe, Scalar, isRecord, isScalar, isUnset, own, validateValue } from './backendSchema';

export type FieldEdit = { mode: 'inherit' } | { mode: 'set'; value: Scalar; draftText?: string };
export interface ModelOptionSnapshot {
  recipe: string;
  model_name: string;
  saved: Record<string, Scalar>;
  effective: Record<string, Scalar>;
  defaults: Record<string, Scalar>;
}
export interface RecipeForm {
  recipe: BackendRecipe;
  snapshot: ModelOptionSnapshot;
  catalogId: string;
  connectionGeneration: number;
  edits: Map<string, FieldEdit>;
  review: boolean;
}
const controls = new Set(['model', 'model_name', 'save_options', 'dry_run']);
export const editableOptions = (recipe: BackendRecipe): BackendOption[] => recipe.options
  .filter(option => !controls.has(option.name) && option.name !== 'pinned')
  .map((option, position) => ({ option, position }))
  .sort((a, b) => (a.option.ui?.order ?? Infinity) - (b.option.ui?.order ?? Infinity) || a.position - b.position)
  .map(({ option }) => option);

export function parseModelOptions(value: unknown, recipe: string, model: string): ModelOptionSnapshot {
  if (!isRecord(value) || value.recipe !== recipe || value.model_name !== model) throw new Error('Model recipe changed; reopen its options.');
  for (const key of ['saved', 'effective', 'defaults']) {
    if (!isRecord(value[key]) || !Object.values(value[key]).every(isScalar)) throw new Error('Malformed model-options response.');
  }
  return value as unknown as ModelOptionSnapshot;
}

export function createForm(recipe: BackendRecipe, snapshot: ModelOptionSnapshot, catalogId: string, connectionGeneration: number): RecipeForm {
  return { recipe, snapshot, catalogId, connectionGeneration, edits: new Map(), review: false };
}
export function reconcileForm(form: RecipeForm, recipe: BackendRecipe, snapshot: ModelOptionSnapshot, catalogId: string): RecipeForm {
  return { ...form, recipe, snapshot, catalogId, review: form.review || form.catalogId !== catalogId };
}
export function fieldValue(form: RecipeForm, option: BackendOption): Scalar {
  const edit = form.edits.get(option.name);
  if (edit?.mode === 'set') return edit.value;
  const value = own(edit?.mode === 'inherit' ? form.snapshot.defaults : form.snapshot.effective, option.name);
  return value === undefined ? option.default_value : value;
}
export function setEdit(form: RecipeForm, name: string, edit: FieldEdit): RecipeForm {
  const edits = new Map(form.edits);
  edits.set(name, edit);
  return { ...form, edits };
}
export function editValue(form: RecipeForm, option: BackendOption, value: Scalar): RecipeForm {
  return setEdit(form, option.name, isUnset(option, value) ? { mode: 'inherit' } : { mode: 'set', value });
}
const numericPattern = /^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$/;
export function numericValue(text: string): number | undefined {
  return numericPattern.test(text.trim()) && Number.isFinite(Number(text)) ? Number(text) : undefined;
}
export function editNumeric(form: RecipeForm, option: BackendOption, draftText: string): RecipeForm {
  const value = numericValue(draftText);
  if (value !== undefined && isUnset(option, value)) return setEdit(form, option.name, { mode: 'inherit' });
  return setEdit(form, option.name, { mode: 'set', value: value ?? fieldValue(form, option), draftText });
}
export function formErrors(form: RecipeForm, backends: string[]): Map<string, string> {
  const errors = new Map<string, string>();
  if (form.recipe.incompatibility) errors.set('', form.recipe.incompatibility);
  const options = new Map(editableOptions(form.recipe).map(option => [option.name, option]));
  for (const [name] of form.edits) if (!options.has(name) && name !== 'pinned' && !controls.has(name)) errors.set(name, 'Edited option was removed by the server; discard it or reopen the form.');
  for (const [name, option] of options) {
    const edit = form.edits.get(name);
    let error: string | undefined;
    if (edit?.mode === 'set' && edit.draftText !== undefined && numericValue(edit.draftText) === undefined) error = 'Enter a complete finite number.';
    else error = validateValue(option, fieldValue(form, option), backends);
    if (option.ui?.choices && backends.length === 0) error = 'No usable backends on the selected server. Check its setup information.';
    if (error) errors.set(name, error);
  }
  return errors;
}
export function mutationPayload(form: RecipeForm, backends: string[]): Record<string, Scalar> {
  if (formErrors(form, backends).size) throw new Error('Resolve option validation errors before submitting.');
  const result: Record<string, Scalar> = Object.create(null);
  for (const [name, edit] of form.edits) {
    if (name === 'pinned' || controls.has(name)) continue;
    result[name] = edit.mode === 'inherit' ? null : edit.value;
  }
  return result;
}
