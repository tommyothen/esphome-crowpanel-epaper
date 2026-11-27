import starlight from '@astrojs/starlight';
import { defineConfig } from 'astro/config';
import { inkTheme } from './src/ink-theme';

const REPO = 'https://github.com/tommyothen/esphome-crowpanel-epaper';

export default defineConfig({
  site: 'https://tommyothen.github.io',
  base: '/esphome-crowpanel-epaper',
  integrations: [
    starlight({
      title: 'esphome-crowpanel-epaper',
      description:
        "ESPHome display drivers for ELECROW's CrowPanel ESP32-S3 e-paper boards: even greys, partials without a flash, glides and a live mode.",
      social: [{ icon: 'github', label: 'GitHub', href: REPO }],
      customCss: ['@fontsource-variable/inter', './src/styles/epaper.css'],
      components: {
        ThemeProvider: './src/components/ThemeProvider.astro',
        ThemeSelect: './src/components/ThemeSelect.astro',
      },
      expressiveCode: {
        themes: [inkTheme],
        useStarlightDarkModeSwitch: false,
        styleOverrides: { borderRadius: '0', borderWidth: '0', frames: { shadowColor: 'transparent' } },
      },
      sidebar: [
        { label: 'Start here', items: [{ label: 'Overview', slug: 'index' }, 'choosing-a-mode'] },
        { label: 'Using it', items: ['guide', { label: 'Examples', link: `${REPO}/tree/main/examples` }] },
        {
          label: 'Under the hood',
          items: [
            'how-it-works',
            'turbo',
            { label: '5.79" notes', slug: 'crowpanel-579' },
            { label: '4.2" notes', slug: 'crowpanel-42' },
          ],
        },
        { label: 'About', items: ['outside-esphome', 'other-drivers'] },
      ],
    }),
  ],
});
