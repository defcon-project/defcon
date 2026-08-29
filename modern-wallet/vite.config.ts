import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Port 4180: distinct from the mock's 4175 so both can run side by side while
// the mock still exists as a visual reference.
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 4180,
    strictPort: true,
  },
  build: {
    target: 'es2022',
  },
})
