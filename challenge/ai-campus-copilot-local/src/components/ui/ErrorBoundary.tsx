"use client";

import { Component, type ErrorInfo, type ReactNode } from "react";

interface Props {
  children: ReactNode;
  fallbackTitle?: string;
}

interface State {
  error: Error | null;
}

export class ErrorBoundary extends Component<Props, State> {
  override state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  override componentDidCatch(error: Error, info: ErrorInfo): void {
    // Local console only. The app ships no telemetry and reports nothing remotely.
    console.error("AI Campus Copilot Local caught an error", error, info.componentStack);
  }

  override render(): ReactNode {
    const { error } = this.state;
    if (!error) return this.props.children;

    return (
      <div className="card p-6" role="alert">
        <h2 className="text-base font-semibold text-ink">
          {this.props.fallbackTitle ?? "Something went wrong on this page"}
        </h2>
        <p className="mt-2 text-sm text-ink-muted">
          The rest of the app still works, and your local documents are untouched.
        </p>
        <pre className="mt-3 overflow-x-auto rounded-lg bg-surface-sunken p-3 font-mono text-xs text-ink-muted">
          {error.message}
        </pre>
        <button
          type="button"
          className="btn-ghost mt-4"
          onClick={() => this.setState({ error: null })}
        >
          Try again
        </button>
      </div>
    );
  }
}
