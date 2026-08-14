// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://giovi321.github.io',
  base: '/led-display',
  integrations: [
    starlight({
      title: 'LED Display',
      social: [
        { icon: 'github', label: 'GitHub', href: 'https://github.com/giovi321/led-display' },
      ],
      sidebar: [
        { label: 'Getting started', items: [{ autogenerate: { directory: 'getting-started' } }] },
        { label: 'User guide', items: [{ autogenerate: { directory: 'user-guide' } }] },
        { label: 'Admin guide', items: [{ autogenerate: { directory: 'admin-guide' } }] },
        { label: 'API reference', items: [{ autogenerate: { directory: 'api-reference' } }] },
        { label: 'Architecture', items: [{ autogenerate: { directory: 'architecture' } }] },
        { label: 'Development', items: [{ autogenerate: { directory: 'development' } }] },
      ],
    }),
  ],
});
