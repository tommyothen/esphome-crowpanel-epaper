// Code blocks in ink and the panels' greys, highlighted by shade rather than colour. A VS Code theme
// object, the form Expressive Code takes.
const INK = '#1d1d1b';
const PAPER = '#eeece6';

export const inkTheme = {
  name: 'ink',
  type: 'dark',
  colors: {
    'editor.background': INK,
    'editor.foreground': PAPER,
    'editor.selectionBackground': '#5b5a57',
    'editorGroupHeader.tabsBackground': '#3b3a38',
    'tab.activeBackground': INK,
    'tab.activeForeground': PAPER,
    'tab.inactiveForeground': '#a1a09b',
    'titleBar.activeBackground': '#3b3a38',
    'titleBar.activeForeground': PAPER,
  },
  tokenColors: [
    { scope: ['comment', 'punctuation.definition.comment'], settings: { foreground: '#a1a09b', fontStyle: 'italic' } },
    { scope: ['string', 'constant', 'constant.numeric', 'constant.language'], settings: { foreground: '#c7c5c0' } },
    { scope: ['punctuation', 'keyword.operator'], settings: { foreground: '#a1a09b' } },
    {
      scope: ['entity.name.tag', 'keyword', 'storage', 'support', 'entity.name', 'variable'],
      settings: { foreground: PAPER, fontStyle: 'bold' },
    },
  ],
};
