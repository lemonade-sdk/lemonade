import React from 'react';
import { formatJsonBlock, normalizeJsonForDisplay } from './utils/requestLogFormat';

export interface TextSection {
  key: string;
  label: string;
  text: string;
}

const TEXT_FIELD_NAMES = new Set([
  'prompt',
  'content',
  'text',
  'input',
  'system',
  'message',
]);

const LONG_TEXT_MIN_LENGTH = 120;

function isRedactionSummary(value: unknown): boolean {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return false;
  }
  const record = value as Record<string, unknown>;
  return 'char_count' in record && Object.keys(record).length <= 2;
}

function isTextFieldName(key: string): boolean {
  return TEXT_FIELD_NAMES.has(key.toLowerCase());
}

function shouldExtractAsText(key: string, value: string): boolean {
  if (isTextFieldName(key)) {
    return true;
  }
  return value.length > LONG_TEXT_MIN_LENGTH || value.includes('\n');
}

function placeholderForText(text: string): string {
  return `[shown below — ${text.length} chars]`;
}

function extractTextSections(value: unknown, path = ''): TextSection[] {
  const sections: TextSection[] = [];

  if (typeof value === 'string') {
    if (path && shouldExtractAsText(path.split('.').pop() ?? '', value)) {
      sections.push({
        key: path,
        label: path,
        text: value,
      });
    }
    return sections;
  }

  if (Array.isArray(value)) {
    value.forEach((item, index) => {
      const childPath = path ? `${path}[${index}]` : `[${index}]`;
      if (item && typeof item === 'object' && !Array.isArray(item)) {
        const record = item as Record<string, unknown>;
        const role =
          typeof record.role === 'string' ? record.role : `item ${index}`;
        if (record.content !== undefined) {
          if (typeof record.content === 'string' && !isRedactionSummary(record.content)) {
            if (shouldExtractAsText('content', record.content)) {
              sections.push({
                key: `${childPath}.content`,
                label: path ? `${path}[${index}] (${role})` : `messages[${index}] (${role})`,
                text: record.content,
              });
            }
          } else if (Array.isArray(record.content)) {
            sections.push(...extractTextSections(record.content, `${childPath}.content`));
          }
        }
        for (const [key, child] of Object.entries(record)) {
          if (key === 'content') continue;
          sections.push(...extractTextSections(child, `${childPath}.${key}`));
        }
      } else {
        sections.push(...extractTextSections(item, childPath));
      }
    });
    return sections;
  }

  if (value && typeof value === 'object') {
    const record = value as Record<string, unknown>;

    if (record.messages && Array.isArray(record.messages)) {
      sections.push(...extractTextSections(record.messages, path ? `${path}.messages` : 'messages'));
    } else if (record.messages && isRedactionSummary(record.messages)) {
      // Redacted summary — do not expand.
    }

    for (const [key, child] of Object.entries(record)) {
      if (key === 'messages') continue;

      const childPath = path ? `${path}.${key}` : key;

      if (typeof child === 'string' && !isRedactionSummary(child)) {
        if (shouldExtractAsText(key, child)) {
          sections.push({
            key: childPath,
            label: childPath,
            text: child,
          });
        }
      } else if (key === 'body' && child && typeof child === 'object') {
        // Keep small response bodies inline in metadata JSON.
        sections.push(...extractTextSections(child, childPath));
      } else {
        sections.push(...extractTextSections(child, childPath));
      }
    }
  }

  return sections;
}

function metadataWithPlaceholders(
  value: unknown,
  sections: TextSection[],
  path = '',
): unknown {
  const sectionKeys = new Set(sections.map((s) => s.key));

  if (typeof value === 'string') {
    const key = path.split('.').pop() ?? path;
    if (sectionKeys.has(path) && shouldExtractAsText(key, value)) {
      return placeholderForText(value);
    }
    return value;
  }

  if (Array.isArray(value)) {
    return value.map((item, index) => {
      const childPath = path ? `${path}[${index}]` : `[${index}]`;
      return metadataWithPlaceholders(item, sections, childPath);
    });
  }

  if (value && typeof value === 'object') {
    const record = value as Record<string, unknown>;
    const result: Record<string, unknown> = {};

    for (const [key, child] of Object.entries(record)) {
      const childPath = path ? `${path}.${key}` : key;

      if (key === 'messages' && Array.isArray(child)) {
        result[key] = child.map((item, index) => {
          const msgPath = `${childPath}[${index}]`;
          if (item && typeof item === 'object' && !Array.isArray(item)) {
            const msg = item as Record<string, unknown>;
            const role = typeof msg.role === 'string' ? msg.role : `item ${index}`;
            const contentPath = `${msgPath}.content`;
            if (typeof msg.content === 'string' && sectionKeys.has(contentPath)) {
              return {
                ...msg,
                content: placeholderForText(msg.content),
              };
            }
            return metadataWithPlaceholders(item, sections, msgPath);
          }
          return metadataWithPlaceholders(item, sections, msgPath);
        });
        continue;
      }

      if (typeof child === 'string' && sectionKeys.has(childPath)) {
        result[key] = placeholderForText(child);
      } else {
        result[key] = metadataWithPlaceholders(child, sections, childPath);
      }
    }

    return result;
  }

  return value;
}

interface RequestLogPayloadViewProps {
  value: unknown;
}

const RequestLogPayloadView: React.FC<RequestLogPayloadViewProps> = ({ value }) => {
  const normalized = normalizeJsonForDisplay(value);
  const sections = extractTextSections(normalized);
  const metadata = metadataWithPlaceholders(normalized, sections);

  return (
    <div className="request-log-payload-view">
      <pre className="request-log-json request-log-json--metadata">
        {formatJsonBlock(metadata)}
      </pre>
      {sections.map((section) => (
        <div key={section.key} className="request-log-text-section">
          <div className="request-log-text-label">{section.label}</div>
          <pre className="request-log-text-value">{section.text}</pre>
        </div>
      ))}
    </div>
  );
};

export default RequestLogPayloadView;
