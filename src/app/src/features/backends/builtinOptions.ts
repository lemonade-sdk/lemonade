import { BackendRecipe, Scalar, isScalar, validateValue } from './backendSchema';

// A null default marks a tri-state option whose type the schema row cannot
// state (the server accepts a boolean there), so it is left to the server.
export function builtinOptionErrors(
  recipe: BackendRecipe | undefined,
  values: Record<string, unknown>,
): Map<string, string> {
  const errors = new Map<string, string>();
  if (!recipe) return errors;
  for (const option of recipe.options) {
    if (!Object.prototype.hasOwnProperty.call(values, option.name)) continue;
    const value = values[option.name];
    if (value === null || value === undefined || option.default_value === null) continue;
    const error = isScalar(value) ? validateValue(option, value as Scalar) : `Expected ${option.value_type}.`;
    if (error) errors.set(option.name, error);
  }
  return errors;
}
