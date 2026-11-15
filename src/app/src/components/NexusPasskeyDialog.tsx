import React, { useEffect, useId, useRef, useState } from 'react';
import { useI18n } from '../i18n';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { cancelNexusPasskey, sendNexusPasskeyPin, type PasskeyPrompt } from '../features/nexus/nexus';
import { WorkspaceActionButton } from './WorkspacePanels';

export function NexusPasskeyDialog({ prompt }: { prompt: PasskeyPrompt }) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const id = useId();
  const [pin, setPin] = useState('');
  const [error, setError] = useState('');
  useFocusTrap(ref, true);
  const cancel = async () => {
    try { await cancelNexusPasskey(); } catch (error) { setError(error instanceof Error ? error.message : String(error)); }
  };
  useEffect(() => {
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') { event.preventDefault(); void cancel(); } };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, []);
  return <div className="nexus-verification-backdrop"><div ref={ref} className="nexus-verification" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
    <h3 id={`${id}-title`}>{t('Verify with your passkey')}</h3><p role="status">{t(prompt.message)}</p>
    {error && <p className="nexus-error" role="alert">{error}</p>}
    {prompt.needs_pin && <form onSubmit={event => {
      event.preventDefault(); const value = pin; setPin(''); setError('');
      void sendNexusPasskeyPin(value).catch(error => setError(error instanceof Error ? error.message : String(error)));
    }}><label htmlFor={`${id}-pin`}>{t('Security key PIN')}</label><input autoFocus id={`${id}-pin`} className="input" type="password" autoComplete="off" minLength={4} maxLength={63} value={pin} onChange={event => setPin(event.target.value)} /><WorkspaceActionButton type="submit" disabled={pin.length < 4}>{t('Continue')}</WorkspaceActionButton></form>}
    <WorkspaceActionButton appearance="quiet" onClick={() => void cancel()}>{t('Cancel verification')}</WorkspaceActionButton>
  </div></div>;
}
