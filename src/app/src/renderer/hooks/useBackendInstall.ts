import { useState, useCallback } from 'react';
import { installBackend, uninstallBackend } from '../utils/backendInstaller';
import { writeClipboard } from '../utils/clipboardUtils';
import { RECIPE_DISPLAY_NAMES } from '../utils/recipeNames';

interface UseBackendInstallOptions {
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

export function useBackendInstall({ showError, showSuccess }: UseBackendInstallOptions) {
  const [installingBackends, setInstallingBackends] = useState<Set<string>>(new Set());

  const handleInstall = useCallback(async (recipe: string, backend: string) => {
    const key = `${recipe}:${backend}`;
    setInstallingBackends(prev => new Set(prev).add(key));
    try {
      const result = await installBackend(recipe, backend, true);
      if (result === 'action') {
        return;
      }
      showSuccess(`${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend} installed successfully.`);
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : 'Unknown error';
      showError(`Failed: ${errorMessage}`);
    } finally {
      setInstallingBackends(prev => {
        const next = new Set(prev);
        next.delete(key);
        return next;
      });
    }
  }, [showError, showSuccess]);

  const handleUninstall = useCallback(async (recipe: string, backend: string) => {
    try {
      await uninstallBackend(recipe, backend);
      showSuccess(`${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend} uninstalled successfully.`);
    } catch (error) {
      showError(`Failed to uninstall backend: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  }, [showError, showSuccess]);

  const handleCopyAction = useCallback(async (recipe: string, backend: string, action?: string) => {
    if (!action) return;
    try {
      // If the action contains a lemonade-server.ai documentation URL, open it in-app
      if (action.match(/https:\/\/lemonade-server\.ai\/[^\s]+\.html/)) {
        const urlMatch = action.match(/https:\/\/lemonade-server\.ai\/[^\s]+/);
        if (urlMatch) {
          window.dispatchEvent(new CustomEvent('open-external-content', { detail: { url: urlMatch[0] } }));
          return;
        }
      }
      await writeClipboard(action);
      showSuccess(`Copied action for ${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend}.`);
    } catch {
      showError('Failed to copy action to clipboard.');
    }
  }, [showError, showSuccess]);

  const isInstalling = useCallback((recipe: string, backend: string) => {
    return installingBackends.has(`${recipe}:${backend}`);
  }, [installingBackends]);

  return { handleInstall, handleUninstall, handleCopyAction, isInstalling };
}
