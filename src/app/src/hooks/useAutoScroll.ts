import { useEffect } from 'react';

export function useAutoScroll(
  refs: React.RefObject<HTMLElement | null>[],
  dependencies: any[],
  activeCondition: boolean
) {
  useEffect(() => {
    if (activeCondition) {
      refs.forEach((ref) => {
        if (ref.current) {
          ref.current.scrollTop = ref.current.scrollHeight;
        }
      });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [...dependencies, activeCondition]);
}
