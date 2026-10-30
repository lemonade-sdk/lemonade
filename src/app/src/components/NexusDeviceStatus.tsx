import React from 'react';
import { useI18n } from '../i18n';

export const nexusDeviceStatusLabel = (online: boolean | undefined) => online === true ? 'Online' : online === false ? 'Offline' : 'Status unknown';

export function NexusDeviceStatus({ online }: { online: boolean | undefined }) {
  const { t } = useI18n();
  const state = online === true ? 'online' : online === false ? 'offline' : 'unknown';
  return <span className={`nexus-device-status nexus-device-status--${state}`}>
    <span className="nexus-device-status__dot" aria-hidden="true" />
    <span>{t(nexusDeviceStatusLabel(online))}</span>
  </span>;
}
