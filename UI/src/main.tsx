import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App'
import { loadPrefs } from './PreferencesPanel'
import { applyTheme } from './Theme'

// Restore before the first render to avoid a dark flash with a saved light theme.
applyTheme(loadPrefs().theme)

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>
)
