import type { InspectToast } from '../../inspectStore';
import type { TranslationParams } from '../../i18n/types';

type Translate = (key: string, params?: TranslationParams) => string;

export function inspectToastText(toast: InspectToast | null, t: Translate): string {
  if (!toast) return '';

  switch (toast.code) {
    case 'capture_unsupported': return t('toasts.captureUnsupported');
    case 'capture_enabled': return t('toasts.captureEnabled');
    case 'capture_paused': return t('toasts.capturePaused');
    case 'session_cleared': return t('toasts.sessionCleared');
    case 'request_deleted': return t('toasts.requestDeleted');
    case 'authentication_failed':
      return toast.message
        ? t('toasts.authenticationFailedDetail', { message: toast.message })
        : t('toasts.authenticationFailed');
    case 'session_exported': return t('toasts.sessionExported', { count: toast.count ?? 0 });
    case 'copied': return toast.label ? t('toasts.copiedLabel', { label: toast.label }) : t('toasts.copied');
    case 'copy_failed': return t('toasts.copyFailed');
    case 'prompt_analysis_complete': return t('toasts.promptAnalysisComplete');
    case 'select_model': return t('toasts.selectModel');
    case 'select_test_model': return t('toasts.selectTestModel');
    case 'test_execution_completed': return t('toasts.testExecutionCompleted');
    case 'completion_succeeded': return t('toasts.completionSucceeded');
    case 'request_failed':
      return toast.message
        ? t('toasts.requestFailedDetail', { message: toast.message })
        : t('toasts.requestFailed');
    default: return toast.message ?? '';
  }
}
