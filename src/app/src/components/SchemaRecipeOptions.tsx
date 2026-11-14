import React, { useId } from 'react';
import { Scalar, controlFor, own } from '../features/backends/backendSchema';
import {
  RecipeForm,
  editNumeric,
  editValue,
  editableOptions,
  fieldValue,
  formErrors,
  setEdit,
} from '../features/backends/recipeForm';
import { useI18n } from '../i18n';

interface Props {
  form: RecipeForm;
  backends: string[];
  disabled?: boolean;
  availability?: string;
  onChange: (form: RecipeForm) => void;
}

const format = (value: Scalar | undefined) => value === undefined ? 'none' : JSON.stringify(value);

export const SchemaRecipeOptions: React.FC<Props> = ({ form, backends, disabled, availability, onChange }) => {
  const { t } = useI18n();
  const prefix = useId();
  const errors = formErrors(form, backends);
  const options = editableOptions(form.recipe);
  return (
    <div className="schema-recipe-options">
      {form.review && (
        <p role="status" className="schema-recipe-options__notice">{t('Server configuration changed; review your edits.')}</p>
      )}
      {form.recipe.incompatibility && <p role="alert" className="schema-recipe-options__error">{form.recipe.incompatibility}</p>}
      {options.length === 0 && <p className="schema-recipe-options__notice">{t('This recipe has no configurable execution options.')}</p>}
      {availability && <p className="schema-recipe-options__notice">{availability}</p>}
      {form.recipe.support.map((support, index) => (
        <p key={index} className="schema-recipe-options__support">
          {support.backend}: {support.tier}; {support.device_summary}; {support.supported_os.join(', ')}
        </p>
      ))}
      <div className="detail-configuration__selector-grid">
        {options.map((option, index) => {
          const id = `${prefix}-${index}`;
          const value = fieldValue(form, option);
          const edit = form.edits.get(option.name);
          const control = controlFor(option);
          const error = errors.get(option.name);
          const choices = option.ui?.choices ? backends : option.enum ?? [];
          const locked = disabled || !!form.recipe.incompatibility;
          const props = { id, disabled: locked, 'aria-invalid': !!error, 'aria-describedby': `${id}-details` };
          const set = (next: Scalar) => onChange(editValue(form, option, next));
          const saved = own(form.snapshot.saved, option.name);
          return (
            <div className="detail-tuning__field schema-recipe-options__field" key={option.name}>
              {option.group && <span className="schema-recipe-options__group">{option.group}</span>}
              <label htmlFor={id}>{option.ui?.label ?? option.name}</label>
              {control === 'checkbox' ? (
                <input {...props} type="checkbox" checked={value === true} onChange={event => set(event.target.checked)} />
              ) : control === 'select' ? (
                <select
                  {...props}
                  className="select detail-configuration__select"
                  value={choices.findIndex(choice => choice === value)}
                  onChange={event => {
                    const choice = choices[Number(event.target.value)];
                    if (choice !== undefined) set(choice);
                  }}
                >
                  <option value={-1} disabled>
                    {format(value)} {choices.length ? t('(inherited or unavailable)') : t('(no usable choices)')}
                  </option>
                  {choices.map((choice, choiceIndex) => <option key={choiceIndex} value={choiceIndex}>{format(choice)}</option>)}
                </select>
              ) : control === 'textarea' ? (
                <textarea
                  {...props}
                  className="input"
                  value={typeof value === 'string' ? value : ''}
                  onChange={event => set(event.target.value)}
                />
              ) : (
                <input
                  {...props}
                  className="input"
                  type={control === 'number' ? 'number' : 'text'}
                  step={option.value_type === 'integer' ? 1 : 'any'}
                  min={option.minimum}
                  max={option.maximum}
                  value={edit?.mode === 'set' && edit.draftText !== undefined ? edit.draftText : value === null ? '' : String(value)}
                  onChange={event => control === 'number'
                    ? onChange(editNumeric(form, option, event.target.value))
                    : set(event.target.value)}
                />
              )}
              <small id={`${id}-details`}>
                {option.help && <span className="schema-recipe-options__line">{option.help}</span>}
                <span className="schema-recipe-options__line">
                  {t('Schema default')}: {format(option.default_value)}; {t('Server default')}: {format(own(form.snapshot.defaults, option.name))}
                </span>
                <span className="schema-recipe-options__line">
                  {t('Effective')}: {format(own(form.snapshot.effective, option.name))}; {t('Saved override')}: {format(saved)}
                  {' '}({edit ? t('draft') : saved === undefined ? t('inherited') : t('saved')})
                </span>
                {(option.minimum !== undefined || option.maximum !== undefined) && (
                  <span className="schema-recipe-options__line">
                    {t('Range')}: {option.minimum ?? '−∞'} – {option.maximum ?? '∞'}
                  </span>
                )}
                {error && <span role="alert" className="schema-recipe-options__line schema-recipe-options__error">{error}</span>}
              </small>
              <button
                type="button"
                className="btn btn--ghost btn--sm schema-recipe-options__inherit"
                disabled={disabled}
                onClick={() => onChange(setEdit(form, option.name, { mode: 'inherit' }))}
              >
                {t('Use inherited value')}
              </button>
            </div>
          );
        })}
      </div>
      {[...form.edits].filter(([name]) => !options.some(option => option.name === name)).map(([name]) => (
        <div key={name} role="alert" className="schema-recipe-options__error">
          {name}: {errors.get(name)}
          <button
            type="button"
            className="btn btn--ghost btn--sm"
            disabled={disabled}
            onClick={() => {
              const edits = new Map(form.edits);
              edits.delete(name);
              onChange({ ...form, edits });
            }}
          >
            {t('Discard removed edit')}
          </button>
        </div>
      ))}
    </div>
  );
};
