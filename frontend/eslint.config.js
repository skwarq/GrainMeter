import js from '@eslint/js';
import globals from 'globals';

export default [
  { ignores: ['node_modules/', '.dev/'] },
  js.configs.recommended,
  {
    files: ['src/**/*.{js,jsx}'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      parserOptions: { ecmaFeatures: { jsx: true } },
      globals: { ...globals.browser },
    },
    rules: {
      // device data must never be injected as HTML
      'no-restricted-properties': [
        'error',
        { property: 'innerHTML', message: 'Render device data as text nodes.' },
        { property: 'outerHTML', message: 'Render device data as text nodes.' },
        { property: 'insertAdjacentHTML', message: 'Render device data as text nodes.' },
      ],
      'no-restricted-syntax': [
        'error',
        {
          selector: "JSXAttribute[name.name='dangerouslySetInnerHTML']",
          message: 'Render device data as text nodes.',
        },
        {
          selector: "CallExpression[callee.name='fetch']",
          message: 'Only src/api.js may call fetch().',
        },
        {
          selector: "NewExpression[callee.name='XMLHttpRequest']",
          message: 'Only src/api.js may use XMLHttpRequest.',
        },
      ],
      'no-unused-vars': ['error', { varsIgnorePattern: '^_', argsIgnorePattern: '^_', caughtErrors: 'none' }],
    },
  },
  {
    // the device API and the dashboard's ThingSpeak reader are the only
    // modules allowed to use the network
    files: ['src/api.js', 'src/dashboard/thingspeak.js'],
    rules: { 'no-restricted-syntax': 'off' },
  },
  {
    files: ['scripts/**/*.mjs', 'test/**/*.js', 'eslint.config.js'],
    languageOptions: { ecmaVersion: 'latest', sourceType: 'module', globals: { ...globals.node } },
  },
];
