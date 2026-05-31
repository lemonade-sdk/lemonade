import React, { useEffect, useMemo, useState } from 'react';
import { AccountRecord, AccountSession, accountCount, clearAllAccountsAndScopedData, clearCurrentSessionData, createAccount, deleteOwnAccount, describeSession, listAccounts, signIn, signOut } from './accountStore';

interface AccountMenuProps {
  session: AccountSession;
  onSessionChange: (session: AccountSession) => void;
  onDataReset: () => void;
}

type PanelMode = 'menu' | 'signin' | 'create' | 'settings';

const AccountMenu: React.FC<AccountMenuProps> = ({ session, onSessionChange, onDataReset }) => {
  const [open, setOpen] = useState(false);
  const [mode, setMode] = useState<PanelMode>('menu');
  const [accounts, setAccounts] = useState<AccountRecord[]>(() => listAccounts());
  const [name, setName] = useState('');
  const [password, setPassword] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  const firstAccountWillBeAdmin = useMemo(() => accountCount() === 0, [open, accounts.length]);

  useEffect(() => { if (open) setAccounts(listAccounts()); }, [open, session]);

  const resetForm = () => { setName(''); setPassword(''); setError(null); setNotice(null); };
  const switchMode = (next: PanelMode) => { resetForm(); setMode(next); };

  const handleCreate = async (e: React.FormEvent) => {
    e.preventDefault();
    setBusy(true); setError(null); setNotice(null);
    try {
      const next = await createAccount(name, password);
      setAccounts(listAccounts());
      onSessionChange(next);
      setNotice(next.role === 'admin' ? 'Account created. First local account is admin.' : 'Account created and signed in.');
      setMode('settings');
    } catch (err) { setError(err instanceof Error ? err.message : 'Could not create account.'); }
    finally { setBusy(false); }
  };

  const handleSignIn = async (e: React.FormEvent) => {
    e.preventDefault();
    setBusy(true); setError(null); setNotice(null);
    try {
      const next = await signIn(name, password);
      onSessionChange(next);
      setNotice(`Signed in as ${next.name}.`);
      setMode('settings');
    } catch (err) { setError(err instanceof Error ? err.message : 'Could not sign in.'); }
    finally { setBusy(false); }
  };

  const handleSignOut = () => {
    onSessionChange(signOut());
    setNotice('Signed out. You are now in the shared guest space.');
    setMode('menu');
    onDataReset();
  };

  const handleClearMine = () => {
    const label = session.isGuest ? 'shared guest data' : `${session.name}'s data`;
    if (!window.confirm(`Delete ${label} on this device? This removes scoped conversations, active chat, presets and custom models for this profile only.`)) return;
    clearCurrentSessionData(session);
    setNotice(session.isGuest ? 'Shared guest data was cleared.' : 'Your local profile data was cleared.');
    onDataReset();
  };

  const handleDeleteAccount = () => {
    if (session.isGuest) return;
    if (!window.confirm(`Delete account "${session.name}" and its local data on this device? Other users are not touched.`)) return;
    const next = deleteOwnAccount(session);
    setAccounts(listAccounts());
    onSessionChange(next);
    setNotice('Account deleted. You are now using the shared guest space.');
    setMode('menu');
    onDataReset();
  };

  const handleAdminClearAll = () => {
    if (session.role !== 'admin') return;
    if (!window.confirm('Admin action: delete all local Lemonade users and all scoped user/guest data on this device?')) return;
    const next = clearAllAccountsAndScopedData();
    setAccounts([]);
    onSessionChange(next);
    setNotice('All local user data was cleared by admin.');
    setMode('menu');
    onDataReset();
  };

  return (
    <div className="account-menu">
      <button className={`account-menu__trigger ${session.isGuest ? '' : 'account-menu__trigger--signed-in'}`} onClick={() => { setOpen(o => !o); setMode('menu'); setError(null); }} aria-haspopup="dialog" aria-expanded={open} title={describeSession(session)}>
        <span className="account-menu__avatar">{session.isGuest ? 'G' : session.name.charAt(0).toUpperCase()}</span>
        <span className="account-menu__name">{session.isGuest ? 'Guest' : session.name}</span>
      </button>

      {open && (
        <div className="account-menu__panel" role="dialog" aria-label="User settings">
          <div className="account-menu__header">
            <div><div className="account-menu__eyebrow">User space</div><div className="account-menu__title">{describeSession(session)}</div></div>
            <button className="account-menu__close" onClick={() => setOpen(false)} aria-label="Close user menu">×</button>
          </div>

          {notice && <div className="account-menu__notice">{notice}</div>}
          {error && <div className="account-menu__error">⚠ {error}</div>}

          {mode === 'menu' && (
            <>
              <p className="account-menu__copy">Guest chats can be used without login. If guest history is saved, it is shared on this browser. Signed-in users get their own scoped local history, presets and custom models.</p>
              <div className="account-menu__actions">
                <button className="btn btn--primary" onClick={() => switchMode('signin')} disabled={accounts.length === 0}>Sign in</button>
                <button className="btn btn--ghost" onClick={() => switchMode('create')}>Create user</button>
                <button className="btn btn--ghost" onClick={() => switchMode('settings')}>Settings</button>
              </div>
              {accounts.length > 0 && <div className="account-menu__accounts"><span className="account-menu__section-label">Local users</span>{accounts.map(a => <span key={a.id} className="account-menu__account-chip">{a.name}{a.role === 'admin' ? ' · admin' : ''}</span>)}</div>}
            </>
          )}

          {mode === 'signin' && (
            <form className="account-menu__form" onSubmit={handleSignIn}>
              <label>Name<input value={name} onChange={e => setName(e.target.value)} autoFocus /></label>
              <label>Password<input value={password} onChange={e => setPassword(e.target.value)} type="password" /></label>
              <div className="account-menu__actions"><button className="btn btn--primary" type="submit" disabled={busy || !name.trim() || !password}>Sign in</button><button className="btn btn--ghost" type="button" onClick={() => switchMode('menu')}>Back</button></div>
            </form>
          )}

          {mode === 'create' && (
            <form className="account-menu__form" onSubmit={handleCreate}>
              <label>Name<input value={name} onChange={e => setName(e.target.value)} autoFocus /></label>
              <label>Password<input value={password} onChange={e => setPassword(e.target.value)} type="password" minLength={8} /></label>
              <p className="account-menu__hint">Passwords are stored as salted PBKDF2 hashes. {firstAccountWillBeAdmin ? 'The first local account becomes admin.' : 'New accounts can delete only their own data.'}</p>
              <div className="account-menu__actions"><button className="btn btn--primary" type="submit" disabled={busy || !name.trim() || password.length < 8}>Create</button><button className="btn btn--ghost" type="button" onClick={() => switchMode('menu')}>Back</button></div>
            </form>
          )}

          {mode === 'settings' && (
            <div className="account-menu__settings">
              <div className="account-menu__setting-row"><span>Current mode</span><strong>{session.isGuest ? 'Shared guest' : session.role}</strong></div>
              <div className="account-menu__setting-row"><span>Storage scope</span><code>{session.storageScope}</code></div>
              <p className="account-menu__hint">This is a client-side prototype guard: local data is namespaced by user. A production extraction should move auth and authorization to the server.</p>
              <div className="account-menu__actions account-menu__actions--stacked">
                <button className="btn btn--ghost" onClick={handleClearMine}>Delete my scoped data</button>
                {!session.isGuest && <button className="btn btn--ghost" onClick={handleSignOut}>Sign out</button>}
                {!session.isGuest && <button className="btn btn--ghost account-menu__danger" onClick={handleDeleteAccount}>Delete my account</button>}
                {session.role === 'admin' && <button className="btn btn--ghost account-menu__danger" onClick={handleAdminClearAll}>Admin: delete all local user data</button>}
                <button className="btn btn--ghost" onClick={() => switchMode('menu')}>Back</button>
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  );
};

export default AccountMenu;
