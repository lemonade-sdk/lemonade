import React, { useEffect, useId, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { type ModelInfo } from '../api';
import { Icon } from './Icon';

function pickerModelName(model: ModelInfo): string {
  return String((model as any).model_name ?? model.name ?? model.id ?? '').trim();
}

function pickerModelLabel(model: ModelInfo): string {
  return String(model.display_name || pickerModelName(model));
}

interface RouterModelPickerProps {
  models: ModelInfo[];
  value: string;
  onChange: (value: string) => void;
  placeholder?: string;
  searchPlaceholder?: string;
  emptyMessage?: string;
  ariaLabel?: string;
}

export const RouterModelPicker: React.FC<RouterModelPickerProps> = ({
  models,
  value,
  onChange,
  placeholder = 'Select model',
  searchPlaceholder = 'Search models',
  emptyMessage = 'No compatible models match this search.',
  ariaLabel = 'Select model',
}) => {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const [activeIndex, setActiveIndex] = useState(-1);
  const rootRef = useRef<HTMLDivElement>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const popoverRef = useRef<HTMLDivElement>(null);
  const searchRef = useRef<HTMLInputElement>(null);
  const [popoverStyle, setPopoverStyle] = useState<React.CSSProperties>({});
  const baseId = useId();
  const listboxId = `${baseId}-listbox`;

  const options = useMemo(() => models
    .map(model => ({ model, name: pickerModelName(model), label: pickerModelLabel(model) }))
    .filter(option => option.name), [models]);

  const selected = useMemo(() => options.find(option => option.name === value), [options, value]);

  const filtered = useMemo(() => {
    const normalized = query.trim().toLowerCase();
    if (!normalized) return options;
    return options.filter(({ model, name, label }) =>
      `${label} ${name} ${(model.labels || []).join(' ')}`.toLowerCase().includes(normalized)
    );
  }, [options, query]);

  useEffect(() => {
    if (!open) return;
    const onPointerDown = (event: MouseEvent) => {
      const target = event.target as Node;
      if (rootRef.current?.contains(target) || popoverRef.current?.contains(target)) return;
      setOpen(false);
    };
    document.addEventListener('mousedown', onPointerDown);
    return () => document.removeEventListener('mousedown', onPointerDown);
  }, [open]);

  useEffect(() => {
    if (!open) return;
    const updatePosition = () => {
      const trigger = triggerRef.current;
      if (!trigger) return;
      const rect = trigger.getBoundingClientRect();
      const viewportPadding = 12;
      const width = Math.min(Math.max(rect.width, 320), Math.max(240, window.innerWidth - viewportPadding * 2));
      const left = Math.max(viewportPadding, Math.min(rect.left, window.innerWidth - width - viewportPadding));
      const below = window.innerHeight - rect.bottom;
      const above = rect.top;
      if (below < 280 && above > below) {
        setPopoverStyle({
          position: 'fixed',
          left,
          bottom: window.innerHeight - rect.top + 6,
          width,
        });
      } else {
        setPopoverStyle({
          position: 'fixed',
          left,
          top: rect.bottom + 6,
          width,
        });
      }
    };
    updatePosition();
    window.addEventListener('resize', updatePosition);
    window.addEventListener('scroll', updatePosition, true);
    return () => {
      window.removeEventListener('resize', updatePosition);
      window.removeEventListener('scroll', updatePosition, true);
    };
  }, [open]);

  useEffect(() => {
    if (!open) {
      setQuery('');
      setActiveIndex(-1);
      return;
    }
    const frame = window.requestAnimationFrame(() => searchRef.current?.focus());
    return () => window.cancelAnimationFrame(frame);
  }, [open]);

  useEffect(() => {
    if (activeIndex >= filtered.length) setActiveIndex(filtered.length ? filtered.length - 1 : -1);
  }, [activeIndex, filtered.length]);

  const closeAndRestoreFocus = () => {
    setOpen(false);
    window.requestAnimationFrame(() => triggerRef.current?.focus());
  };

  const choose = (name: string) => {
    onChange(name);
    closeAndRestoreFocus();
  };

  const onSearchKeyDown = (event: React.KeyboardEvent<HTMLInputElement>) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      closeAndRestoreFocus();
      return;
    }
    if (event.key === 'ArrowDown') {
      event.preventDefault();
      setActiveIndex(index => filtered.length ? (index + 1) % filtered.length : -1);
      return;
    }
    if (event.key === 'ArrowUp') {
      event.preventDefault();
      setActiveIndex(index => filtered.length ? (index <= 0 ? filtered.length - 1 : index - 1) : -1);
      return;
    }
    if (event.key === 'Enter' && filtered.length) {
      event.preventDefault();
      choose(filtered[Math.max(0, activeIndex)].name);
    }
  };

  return (
    <div className="router-model-picker" ref={rootRef}>
      <button
        ref={triggerRef}
        type="button"
        className={`router-model-picker__trigger ${open ? 'is-open' : ''}`}
        aria-label={ariaLabel}
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-controls={open ? listboxId : undefined}
        onClick={() => setOpen(current => !current)}
      >
        <span className={value ? '' : 'is-placeholder'} title={value || undefined}>
          {selected ? `${selected.label} · ${selected.name}` : value || placeholder}
        </span>
        <Icon name="chevron-down" size={14} aria-hidden="true" />
      </button>

      {open && createPortal(
        <div ref={popoverRef} className="router-model-picker__popover" style={popoverStyle}>
          <div className="router-model-picker__search">
            <Icon name="search" size={14} aria-hidden="true" />
            <input
              ref={searchRef}
              type="search"
              value={query}
              placeholder={searchPlaceholder}
              role="combobox"
              aria-label={searchPlaceholder}
              aria-autocomplete="list"
              aria-expanded="true"
              aria-controls={listboxId}
              aria-activedescendant={activeIndex >= 0 ? `${baseId}-option-${activeIndex}` : undefined}
              onChange={event => { setQuery(event.target.value); setActiveIndex(-1); }}
              onKeyDown={onSearchKeyDown}
            />
          </div>
          <div className="router-model-picker__options" id={listboxId} role="listbox" aria-label={ariaLabel}>
            {filtered.map((option, index) => (
              <button
                type="button"
                key={option.name}
                id={`${baseId}-option-${index}`}
                role="option"
                aria-selected={option.name === value}
                className={`router-model-picker__option ${index === activeIndex ? 'is-active' : ''} ${option.name === value ? 'is-selected' : ''}`}
                onMouseEnter={() => setActiveIndex(index)}
                onClick={() => choose(option.name)}
              >
                <span>
                  <strong>{option.label}</strong>
                  <small>{option.name}</small>
                </span>
                {option.name === value && <Icon name="check" size={14} aria-hidden="true" />}
              </button>
            ))}
            {filtered.length === 0 && <div className="router-model-picker__empty">{emptyMessage}</div>}
          </div>
          {value && !selected && (
            <div className="router-model-picker__stale" role="note">
              Current value <code>{value}</code> is not in the compatible model list. Choose another model to replace it.
            </div>
          )}
        </div>,
        document.body,
      )}
    </div>
  );
};

export default RouterModelPicker;
