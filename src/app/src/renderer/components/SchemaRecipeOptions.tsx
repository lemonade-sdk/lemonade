import React, { useId } from 'react';
import { Scalar, controlFor, own } from '../recipes/backendSchema';
import { RecipeForm, editNumeric, editValue, editableOptions, fieldValue, formErrors, setEdit } from '../recipes/recipeForm';

interface Props {
  form: RecipeForm;
  backends: string[];
  disabled?: boolean;
  availability?: string;
  onChange: (form: RecipeForm) => void;
}
const format = (value: Scalar | undefined) => value === undefined ? 'none' : JSON.stringify(value);
export const SchemaRecipeOptions: React.FC<Props> = ({ form, backends, disabled, availability, onChange }) => {
  const prefix = useId();
  const errors = formErrors(form, backends);
  const options = editableOptions(form.recipe);
  return <div className="schema-recipe-options">
    {form.review && <p role="status" className="schema-notice">Server configuration changed; review your edits.</p>}
    {form.recipe.incompatibility && <p role="alert">{form.recipe.incompatibility}</p>}
    {options.length === 0 && <p>This recipe has no configurable execution options.</p>}
    {availability && <p className="schema-notice">{availability}</p>}
    {form.recipe.support.map((support, index) => <p key={index} className="settings-description">
      {support.backend}: {support.tier}; {support.device_summary}; {support.supported_os.join(', ')}
    </p>)}
    {options.map((option, index) => {
      const id = `${prefix}-${index}`;
      const value = fieldValue(form, option);
      const edit = form.edits.get(option.name);
      const control = controlFor(option);
      const error = errors.get(option.name);
      const choices = option.ui?.choices ? backends : option.enum ?? [];
      const props = { id, disabled: disabled || !!form.recipe.incompatibility,
        'aria-invalid': !!error, 'aria-describedby': `${id}-details`, className: 'form-input' };
      const set = (next: Scalar) => onChange(editValue(form, option, next));
      return <div className="form-section" key={option.name}>
        {option.group && <div className="schema-group">{option.group}</div>}
        <label className="form-label" htmlFor={id}>{option.ui?.label ?? option.name}</label>
        {control === 'checkbox' ? <input {...props} type="checkbox" checked={value === true}
          onChange={event => set(event.target.checked)} /> :
          control === 'select' ? <select {...props} value={choices.findIndex(choice => choice === value)}
            onChange={event => { const choice = choices[Number(event.target.value)]; if (choice !== undefined) set(choice); }}>
            <option value={-1} disabled>{format(value)} {choices.length ? '(inherited or unavailable)' : '(no usable choices)'}</option>
            {choices.map((choice, choiceIndex) => <option key={choiceIndex} value={choiceIndex}>{format(choice)}</option>)}
          </select> : control === 'textarea' ? <textarea {...props} value={typeof value === 'string' ? value : ''}
            onChange={event => set(event.target.value)} /> :
          <input {...props} type={control === 'number' ? 'number' : 'text'}
            step={option.value_type === 'integer' ? 1 : 'any'} min={option.minimum} max={option.maximum}
            value={edit?.mode === 'set' && edit.draftText !== undefined ? edit.draftText : value === null ? '' : String(value)}
            onChange={event => control === 'number' ? onChange(editNumeric(form, option, event.target.value)) : set(event.target.value)} />}
        <div id={`${id}-details`} className="settings-description">
          {option.help && <div>{option.help}</div>}
          <div>Schema default: {format(option.default_value)}; server default: {format(own(form.snapshot.defaults, option.name))}</div>
          <div>Effective: {format(own(form.snapshot.effective, option.name))}; saved override: {format(own(form.snapshot.saved, option.name))}
            {' '}({edit ? 'draft' : own(form.snapshot.saved, option.name) === undefined ? 'inherited' : 'saved'})</div>
          {(option.minimum !== undefined || option.maximum !== undefined) && <div>Range: {option.minimum ?? '−∞'} to {option.maximum ?? '∞'}</div>}
          {error && <div role="alert" className="schema-error">{error}</div>}
        </div>
        <button type="button" className="settings-field-reset" disabled={disabled}
          onClick={() => onChange(setEdit(form, option.name, { mode: 'inherit' }))}>Use inherited value</button>
      </div>;
    })}
    {[...form.edits].filter(([name]) => !options.some(option => option.name === name)).map(([name]) =>
      <div key={name} role="alert" className="schema-error">{name}: {errors.get(name)}
        <button type="button" disabled={disabled} onClick={() => {
          const edits = new Map(form.edits); edits.delete(name); onChange({ ...form, edits });
        }}>Discard removed edit</button>
      </div>)}
  </div>;
};
