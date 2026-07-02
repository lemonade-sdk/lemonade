import React, { useState } from 'react';

export interface ModelOption {
  id: string;
  label: string;
  sublabel?: string;
  downloaded?: boolean;
}


interface ModelCheckboxListProps {
  options: ModelOption[];
  selected: string[];
  onToggle: (id: string) => void;
  placeholder?: string;
}

export const ModelCheckboxList: React.FC<ModelCheckboxListProps> = ({
  options, selected, onToggle, placeholder = 'Search models…',
}) => {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const q = query.toLowerCase();
  const filtered = q ? options.filter(o => o.label.toLowerCase().includes(q) || o.id.toLowerCase().includes(q)) : options;

  const summary = selected.length === 0
    ? 'None selected'
    : selected.length === 1
      ? (options.find(o => o.id === selected[0])?.label ?? selected[0])
      : `${selected.length} selected`;

  return (
    <div className={`msearch-container${open ? ' msearch-container--open' : ''}`}>
      <button type="button" className="msearch-toggle" onClick={() => setOpen(v => !v)}>
        <span className="msearch-toggle-summary">{summary}</span>
        <span className="msearch-toggle-arrow">{open ? '▲' : '▼'}</span>
      </button>
      {open && (
        <>
          <div className="msearch-bar">
            <input
              type="text"
              className="msearch-input"
              placeholder={placeholder}
              value={query}
              onChange={e => setQuery(e.target.value)}
              autoFocus
            />
            {query && (
              <button type="button" className="msearch-clear" onClick={() => setQuery('')}>×</button>
            )}
          </div>
          <div className="msearch-list">
            {filtered.length === 0 && (
              <div className="msearch-empty">No models match</div>
            )}
            {filtered.map(o => (
              <label key={o.id} className="router-candidate-row msearch-row">
                <input type="checkbox" checked={selected.includes(o.id)} onChange={() => onToggle(o.id)} />
                <span className="router-candidate-name">{o.label}</span>
                {o.sublabel && (
                  <span className={`router-candidate-badge${o.downloaded ? ' downloaded' : ''}`}>
                    {o.sublabel}
                  </span>
                )}
              </label>
            ))}
          </div>
        </>
      )}
    </div>
  );
};


interface ModelSelectProps {
  options: ModelOption[];
  value: string;
  onChange: (id: string) => void;
  placeholder?: string;
  searchPlaceholder?: string;
  disabled?: boolean;
  annotate?: (id: string) => string | null;
}

export const ModelSelect: React.FC<ModelSelectProps> = ({
  options, value, onChange, placeholder = 'Select model…',
  searchPlaceholder = 'Search models…', disabled, annotate,
}) => {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const q = query.toLowerCase();
  const filtered = q ? options.filter(o => o.label.toLowerCase().includes(q) || o.id.toLowerCase().includes(q)) : options;

  if (disabled) {
    return (
      <select className="form-input form-select" disabled>
        <option>{placeholder}</option>
      </select>
    );
  }

  const selectedLabel = value
    ? (options.find(o => o.id === value)?.label ?? value)
    : placeholder;

  const select = (id: string) => {
    onChange(id);
    setOpen(false);
    setQuery('');
  };

  return (
    <div className={`msearch-container${open ? ' msearch-container--open' : ''}`}>
      <button type="button" className="msearch-toggle" onClick={() => setOpen(v => !v)}>
        <span className={`msearch-toggle-summary${!value ? ' msearch-toggle-summary--placeholder' : ''}`}>
          {selectedLabel}
        </span>
        <span className="msearch-toggle-arrow">{open ? '▲' : '▼'}</span>
      </button>
      {open && (
        <>
          <div className="msearch-bar">
            <input
              type="text"
              className="msearch-input"
              placeholder={searchPlaceholder}
              value={query}
              onChange={e => setQuery(e.target.value)}
              autoFocus
            />
            {query && (
              <button type="button" className="msearch-clear" onClick={() => setQuery('')}>×</button>
            )}
          </div>
          <div className="msearch-list msearch-list--select">
            {filtered.length === 0 && (
              <div className="msearch-empty">No models match</div>
            )}
            {!q && (
              <div
                className={`msearch-option${value === '' ? ' msearch-option--selected' : ''}`}
                onClick={() => select('')}
              >
                <span className="msearch-option-label msearch-option-placeholder">{placeholder}</span>
              </div>
            )}
            {filtered.map(o => {
              const annotation = annotate ? annotate(o.id) : null;
              return (
                <div
                  key={o.id}
                  className={`msearch-option${value === o.id ? ' msearch-option--selected' : ''}`}
                  onClick={() => select(o.id)}
                >
                  <span className="msearch-option-label">{o.label}{annotation ? ` ${annotation}` : ''}</span>
                  {o.sublabel && (
                    <span className={`router-candidate-badge${o.downloaded ? ' downloaded' : ''}`} style={{ marginLeft: 6 }}>
                      {o.sublabel}
                    </span>
                  )}
                </div>
              );
            })}
          </div>
        </>
      )}
    </div>
  );
};
