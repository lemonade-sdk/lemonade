import React from 'react';
import { useI18n } from '../i18n';

export interface ExternalCheckpointFieldsProps {
  roles: string[];
  values: Record<string, string>;
  onChange: (values: Record<string, string>) => void;
}

export function ExternalCheckpointFields({ roles, values, onChange }: ExternalCheckpointFieldsProps) {
  const { t } = useI18n();
  return (
    <>
      {roles.map(role => (
        <label key={role} className="custom-model-form__field custom-model-form__wide">
          <span>{t('Checkpoint for role')}: <code>{role}</code></span>
          <input
            className="input"
            value={values[role] || ''}
            onChange={e => onChange({ ...values, [role]: e.target.value })}
            placeholder={t('Checkpoint accepted by this server recipe')}
          />
        </label>
      ))}
    </>
  );
}
