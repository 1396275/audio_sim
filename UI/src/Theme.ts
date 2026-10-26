import { useSyncExternalStore } from 'react';

export type Theme = 'dark' | 'light';

const THEME_EVENT = 'patchy:theme-change';
const colors = new Map<string, string>();
const currentTheme = (): Theme => document.documentElement.dataset.theme === 'light' ? 'light' : 'dark';

export function applyTheme(theme: Theme) {
  const changed = currentTheme() !== theme;
  document.documentElement.dataset.theme = theme;
  document.documentElement.style.colorScheme = theme;
  colors.clear();
  if (changed) window.dispatchEvent(new Event(THEME_EVENT));
}

function subscribe(listener: () => void) {
  window.addEventListener(THEME_EVENT, listener);
  return () => window.removeEventListener(THEME_EVENT, listener);
}

// Resolved colours and static canvases must refresh even in memoized nodes.
export function useTheme() {
  return useSyncExternalStore(subscribe, currentTheme);
}

// Canvas cannot resolve var(). Cache root tokens so animation loops don't
// trigger a computed-style read for every colour on every frame.
export function themeColor(name: string): string {
  let color = colors.get(name);
  if (color === undefined) {
    color = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    colors.set(name, color);
  }
  return color;
}
