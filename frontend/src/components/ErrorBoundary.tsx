import React from 'react';

interface Props {
  /** When this value changes, any caught error is cleared. */
  resetKey?: string | number;
  children: React.ReactNode;
}

interface State {
  hasError: boolean;
}

/**
 * Catches render errors so a single failure doesn't blank the whole UI.
 * Pass a `resetKey` that changes when the underlying content changes
 * (e.g. node status) so the boundary recovers automatically.
 */
export class ErrorBoundary extends React.Component<Props, State> {
  state: State = { hasError: false };

  static getDerivedStateFromError(): State {
    return { hasError: true };
  }

  componentDidUpdate(prevProps: Props) {
    if (prevProps.resetKey !== this.props.resetKey && this.state.hasError) {
      this.setState({ hasError: false });
    }
  }

  componentDidCatch(error: unknown) {
    console.error('Render error caught by boundary:', error);
  }

  render() {
    if (this.state.hasError) {
      return (
        <div className="p-8 flex flex-col items-center justify-center text-center gap-4 min-h-[300px]">
          <div className="font-mono text-sm uppercase tracking-wider text-red-500">
            RENDER_ERROR
          </div>
          <div className="text-xs text-gray-500 font-mono">
            This content could not be displayed. Try regenerating.
          </div>
        </div>
      );
    }
    return this.props.children;
  }
}
