import { defineConfig } from 'vitepress'

export default defineConfig({
  title: "GSSK",
  description: "General Systems Simulation Kernel Documentation",
  base: '/GSSK/',
  head: [
    ['link', { rel: 'icon', href: '/favicon.png' }]
  ],
  themeConfig: {
    nav: [
      { text: 'Home', link: '/' },
      { text: 'Spec', link: '/SPECIFICATION' },
      { text: 'WASM Demo', link: 'https://sholtomaud.github.io/GSSK/demo/', target: '_blank' }
    ],
    sidebar: [
      {
        text: 'Guides',
        items: [
          { text: 'Concepts', link: '/concepts' },
          { text: 'Cookbook', link: '/cookbook' },
          { text: 'Changelog', link: '/CHANGELOG' },
        ]
      },
      {
        text: 'Reference',
        items: [
          { text: 'API Reference', link: '/api-reference' },
          { text: 'Schema Reference', link: '/gssk-schema' },
          { text: 'LIMIT Logic Bounds', link: '/LIMIT_LOGIC' },
          { text: 'Technical Specification', link: '/SPECIFICATION' },
          { text: 'Requirements', link: '/REQUIREMENTS' },
          { text: 'GPU Acceleration', link: '/DOCS_GPU' },
          { text: 'Performance & Scaling', link: '/DOCS_PERFORMANCE' },
          { text: 'UI Specification', link: '/GSSK_UI_SPECIFICATION' },
        ]
      },
      {
        text: 'Examples',
        items: [
          { text: 'Household Model', link: '/examples/household/' },
        ]
      }
    ],
    socialLinks: [
      { icon: 'github', link: 'https://github.com/sholtomaud/GSSK' }
    ]
  }
})
