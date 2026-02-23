import { defineConfig } from 'vitepress'

export default defineConfig({
  title: "GSSK",
  description: "General Systems Simulation Kernel Documentation",
  base: '/GSSK/',
  themeConfig: {
    nav: [
      { text: 'Home', link: '/' },
      { text: 'Spec', link: '/SPECIFICATION' },
      { text: 'WASM Demo', link: '/demo/', target: '_blank' }
    ],
    sidebar: [
      {
        text: 'Documentation',
        items: [
          { text: 'Requirements', link: '/REQUIREMENTS' },
          { text: 'Technical Specification', link: '/SPECIFICATION' },
          { text: 'GPU Acceleration', link: '/DOCS_GPU' },
          { text: 'Performance & Scaling', link: '/DOCS_PERFORMANCE' },
          { text: 'UI Specification', link: '/GSSK_UI_SPECIFICATION' }
        ]
      }
    ],
    socialLinks: [
      { icon: 'github', link: 'https://github.com/sholtomaud/GSSK' }
    ]
  }
})
