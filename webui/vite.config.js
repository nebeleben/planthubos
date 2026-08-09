import { defineConfig } from 'vite'
import preact from '@preact/preset-vite'

export default defineConfig({
  plugins: [preact()],
  build: {
    // The header logo PNGs (up to ~10.7 KB) must be inlined as base64 data:
    // URIs rather than emitted as separate dist files -- like index.html's
    // favicon comment explains, the hub's webserver only ever embeds/serves
    // app.js/app.css/index.html, so a real logo-*.png file in dist/ would
    // 404 on-device. Default assetsInlineLimit (4096 B) would leave these
    // two above threshold; 16 KiB covers both with headroom without being
    // so high it'd silently swallow some future much-larger asset into the
    // JS bundle. (Also avoids colliding with the assetFileNames: 'app.css'
    // rule below, which every non-inlined asset would otherwise be forced
    // through.)
    assetsInlineLimit: 16 * 1024,
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
