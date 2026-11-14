import js from '@eslint/js';
import globals from 'globals';

export default [
  { ignores: ['node_modules/**', 'dist/**'] },
  js.configs.recommended,
  {
    files: ['src/**/*.js'],
    languageOptions: {
      globals: {
        ...globals.browser,
        ...globals.worker,
        GPUBufferUsage: 'readonly',
        GPUShaderStage: 'readonly',
        GPUMapMode: 'readonly',
        MLGraphBuilder: 'readonly',
        MLTensorUsage: 'readonly',
      },
    },
  },
  { files: ['*.config.js', 'scripts/**/*.mjs'], languageOptions: { globals: { ...globals.node, ...globals.browser } } },
];
