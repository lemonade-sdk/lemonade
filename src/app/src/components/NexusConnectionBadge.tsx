import React from 'react';
import { nexusConnectionLabel, type NexusStatus } from '../features/nexus/nexus';
import { useI18n } from '../i18n';

export function NexusConnectionBadge({ status, nodeId }: { status?: NexusStatus | null; nodeId: string }) {
  const { t } = useI18n();
  const label = nexusConnectionLabel(status, nodeId);
  if (!label) return null;
  return <span className="nexus-connection-badge" title={label === 'P2P' ? t('Direct peer-to-peer connection') : t('Connection through a relay server')}>{t(label)}</span>;
}
