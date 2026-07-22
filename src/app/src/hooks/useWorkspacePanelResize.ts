import React, { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';

const DEFAULT_RAIL_WIDTH = 248;
const COLLAPSED_RAIL_WIDTH = 56;
const LIST_PANEL_MIN_WIDTH = 300;
const LIST_PANEL_MAX_WIDTH = 500;
const DETAIL_PANEL_MIN_WIDTH = 420;

interface WorkspacePanelResizeOptions {
  storageKey: string;
  railCollapsed: boolean;
}

interface WorkspacePanelWidthState {
  width: number;
  customized: boolean;
}

function layoutWidths(containerWidth: number, railWidth: number) {
  const availableWidth = Math.max(0, containerWidth - railWidth);
  const maxWidth = Math.max(
    LIST_PANEL_MIN_WIDTH,
    Math.min(LIST_PANEL_MAX_WIDTH, availableWidth - DETAIL_PANEL_MIN_WIDTH),
  );
  const balancedWidth = Math.max(
    LIST_PANEL_MIN_WIDTH,
    Math.min(maxWidth, Math.round(availableWidth / 2)),
  );
  return { balancedWidth, maxWidth };
}

function initialPanelWidth(storageKey: string, railCollapsed: boolean): WorkspacePanelWidthState {
  const containerWidth = typeof window === 'undefined' ? 1440 : window.innerWidth;
  const railWidth = railCollapsed ? COLLAPSED_RAIL_WIDTH : DEFAULT_RAIL_WIDTH;
  const { balancedWidth, maxWidth } = layoutWidths(containerWidth, railWidth);
  if (typeof window === 'undefined') return { width: balancedWidth, customized: false };

  try {
    const storedWidth = Number(window.localStorage.getItem(storageKey));
    if (Number.isFinite(storedWidth) && storedWidth > 0) {
      return {
        width: Math.max(LIST_PANEL_MIN_WIDTH, Math.min(maxWidth, Math.round(storedWidth))),
        customized: true,
      };
    }
  } catch {
    return { width: balancedWidth, customized: false };
  }

  return { width: balancedWidth, customized: false };
}

export function useWorkspacePanelResize<T extends HTMLElement = HTMLElement>({ storageKey, railCollapsed }: WorkspacePanelResizeOptions) {
  const containerRef = useRef<T>(null);
  const [panelWidth, setPanelWidth] = useState<WorkspacePanelWidthState>(() => initialPanelWidth(storageKey, railCollapsed));
  const panelWidthRef = useRef(panelWidth);

  useEffect(() => {
    panelWidthRef.current = panelWidth;
    if (!panelWidth.customized) return;
    try {
      window.localStorage.setItem(storageKey, String(panelWidth.width));
    } catch {
      return;
    }
  }, [panelWidth, storageKey]);

  const measureLayout = useCallback(() => {
    const container = containerRef.current;
    const containerWidth = container?.getBoundingClientRect().width || window.innerWidth;
    const railWidth = container
      ?.querySelector<HTMLElement>(':scope > .workspace-rail')
      ?.getBoundingClientRect().width
      || (railCollapsed ? COLLAPSED_RAIL_WIDTH : DEFAULT_RAIL_WIDTH);
    return layoutWidths(containerWidth, railWidth);
  }, [railCollapsed]);

  useLayoutEffect(() => {
    const updateForLayout = () => {
      const { balancedWidth, maxWidth } = measureLayout();
      setPanelWidth(current => {
        const width = current.customized
          ? Math.max(LIST_PANEL_MIN_WIDTH, Math.min(maxWidth, current.width))
          : balancedWidth;
        return width === current.width ? current : { ...current, width };
      });
    };

    updateForLayout();
    const observer = typeof ResizeObserver === 'undefined'
      ? null
      : new ResizeObserver(updateForLayout);
    if (containerRef.current) observer?.observe(containerRef.current);
    window.addEventListener('resize', updateForLayout);
    return () => {
      observer?.disconnect();
      window.removeEventListener('resize', updateForLayout);
    };
  }, [measureLayout]);

  const setCustomizedWidth = useCallback((width: number) => {
    const { maxWidth } = measureLayout();
    setPanelWidth({
      width: Math.max(LIST_PANEL_MIN_WIDTH, Math.min(maxWidth, Math.round(width))),
      customized: true,
    });
  }, [measureLayout]);

  const onPointerDown = useCallback((event: React.PointerEvent<HTMLDivElement>) => {
    if (window.innerWidth <= 760) return;
    event.preventDefault();

    const startX = event.clientX;
    const startWidth = containerRef.current
      ?.querySelector<HTMLElement>(':scope > .workspace-list-panel')
      ?.getBoundingClientRect().width
      || panelWidthRef.current.width;
    const handle = event.currentTarget;
    try { handle.setPointerCapture(event.pointerId); } catch {}

    const onPointerMove = (moveEvent: PointerEvent) => {
      setCustomizedWidth(startWidth + moveEvent.clientX - startX);
    };
    const stopResize = () => {
      window.removeEventListener('pointermove', onPointerMove);
      window.removeEventListener('pointerup', stopResize);
      window.removeEventListener('pointercancel', stopResize);
      document.body.classList.remove('is-resizing-workspace-panel');
      try { handle.releasePointerCapture(event.pointerId); } catch {}
    };

    document.body.classList.add('is-resizing-workspace-panel');
    window.addEventListener('pointermove', onPointerMove);
    window.addEventListener('pointerup', stopResize, { once: true });
    window.addEventListener('pointercancel', stopResize, { once: true });
  }, [setCustomizedWidth]);

  const onKeyDown = useCallback((event: React.KeyboardEvent<HTMLDivElement>) => {
    const step = event.shiftKey ? 40 : 16;
    if (event.key === 'ArrowLeft') {
      event.preventDefault();
      setCustomizedWidth(panelWidthRef.current.width - step);
    } else if (event.key === 'ArrowRight') {
      event.preventDefault();
      setCustomizedWidth(panelWidthRef.current.width + step);
    } else if (event.key === 'Home') {
      event.preventDefault();
      setCustomizedWidth(LIST_PANEL_MIN_WIDTH);
    } else if (event.key === 'End') {
      event.preventDefault();
      setCustomizedWidth(measureLayout().maxWidth);
    }
  }, [measureLayout, setCustomizedWidth]);

  const style = useMemo(() => ({
    '--workspace-list-panel-width': `${panelWidth.width}px`,
  } as React.CSSProperties), [panelWidth.width]);

  return {
    containerRef,
    style,
    resizerProps: {
      minWidth: LIST_PANEL_MIN_WIDTH,
      maxWidth: LIST_PANEL_MAX_WIDTH,
      value: panelWidth.width,
      onPointerDown,
      onKeyDown,
    },
  };
}
