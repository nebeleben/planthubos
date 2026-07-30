import { defineConfig } from 'vite'
import preact from '@preact/preset-vite'

export default defineConfig({
  plugins: [preact()],
  build: {
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        chunkFileNames: 'app.js',
        assetFileNames: 'app.css',
      },
    },
  },
  server: {
    proxy: {
      '/api': `http://${process.env.HUB_IP || '192.168.4.1'}`,
    },
  },
})
