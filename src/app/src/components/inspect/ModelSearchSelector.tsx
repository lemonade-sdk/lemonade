import React, { useState, useEffect, useRef, useId } from 'react';
import { type ModelInfo } from '../../api';

interface ModelSearchSelectorProps {
  label: string;
  placeholder?: string;
  value: string;
  onChange: (val: string) => void;
  availableModels: ModelInfo[];
  className?: string;
}

export default function ModelSearchSelector({
  label,
  placeholder = 'Search model...',
  value,
  onChange,
  availableModels,
  className = 'critique-input-control'
}: ModelSearchSelectorProps) {
  const [search, setSearch] = useState('');
  const [isOpen, setIsOpen] = useState(false);
  const [activeIndex, setActiveIndex] = useState(-1);
  const containerRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const isSelectingItemRef = useRef(false);

  const baseId = useId();
  const listboxId = `${baseId}-listbox`;
  const labelId = `${baseId}-label`;

  const filtered = availableModels.filter((m) =>
    (m.name || m.id || '').toLowerCase().includes(search.toLowerCase())
  );

  // Close dropdown on click outside
  useEffect(() => {
    function handleClickOutside(event: MouseEvent) {
      if (containerRef.current && !containerRef.current.contains(event.target as Node)) {
        setIsOpen(false);
      }
    }
    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

  // Reset search when dropdown closes or value changes, syncing query if needed
  useEffect(() => {
    if (!isOpen) {
      if (!isSelectingItemRef.current) {
        if (search) {
          const matchedModel = availableModels.find(
            (m) => (m.name || m.id || '').toLowerCase() === search.toLowerCase()
          );
          if (matchedModel) {
            onChange(matchedModel.name || matchedModel.id || '');
          } else if (search.toLowerCase() !== value.toLowerCase()) {
            onChange('');
          }
        }
      }
      isSelectingItemRef.current = false;
      setSearch('');
      setActiveIndex(-1);
    }
  }, [isOpen, search, value, availableModels, onChange]);

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (!isOpen) {
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        setIsOpen(true);
        e.preventDefault();
      }
      return;
    }

    if (e.key === 'ArrowDown') {
      e.preventDefault();
      setActiveIndex((prev) => (prev + 1 < filtered.length ? prev + 1 : 0));
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      setActiveIndex((prev) => (prev - 1 >= 0 ? prev - 1 : filtered.length - 1));
    } else if (e.key === 'Enter') {
      e.preventDefault();
      if (activeIndex >= 0 && activeIndex < filtered.length) {
        const selected = filtered[activeIndex];
        const val = selected.name || selected.id || '';
        isSelectingItemRef.current = true;
        onChange(val);
        setIsOpen(false);
        inputRef.current?.blur();
      } else if (filtered.length > 0) {
        const selected = filtered[0];
        const val = selected.name || selected.id || '';
        isSelectingItemRef.current = true;
        onChange(val);
        setIsOpen(false);
        inputRef.current?.blur();
      } else {
        isSelectingItemRef.current = true;
        onChange('');
        setIsOpen(false);
        inputRef.current?.blur();
      }
    } else if (e.key === 'Escape') {
      e.preventDefault();
      setIsOpen(false);
      inputRef.current?.focus();
    }
  };

  return (
    <div ref={containerRef} className="flex-col gap-4 model-search-wrapper" style={{ position: 'relative' }}>
      <label id={labelId} className="input-label" htmlFor={baseId}>
        {label}
      </label>
      <input
        ref={inputRef}
        id={baseId}
        type="text"
        placeholder={placeholder}
        value={isOpen ? search : value}
        onChange={(e) => {
          setSearch(e.target.value);
          if (!isOpen) setIsOpen(true);
        }}
        onFocus={() => {
          setSearch(value || '');
          setIsOpen(true);
          setTimeout(() => {
            inputRef.current?.select();
          }, 0);
        }}
        onKeyDown={handleKeyDown}
        className={className}
        autoComplete="off"
        role="combobox"
        aria-expanded={isOpen}
        aria-autocomplete="list"
        aria-controls={listboxId}
        aria-haspopup="listbox"
        aria-activedescendant={
          activeIndex >= 0 && isOpen ? `${baseId}-option-${activeIndex}` : undefined
        }
      />
      {isOpen && filtered.length > 0 && (
        <div id={listboxId} className="model-search-results" role="listbox" aria-labelledby={labelId}>
          {filtered.map((model, idx) => {
            const modelName = model.name || model.id || '';
            return (
              <div
                key={model.id || idx}
                id={`${baseId}-option-${idx}`}
                role="option"
                aria-selected={activeIndex === idx}
                className={`model-search-item ${activeIndex === idx ? 'focused' : ''}`}
                onClick={() => {
                  isSelectingItemRef.current = true;
                  onChange(modelName);
                  setIsOpen(false);
                }}
                style={{ cursor: 'pointer' }}
              >
                {modelName}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
