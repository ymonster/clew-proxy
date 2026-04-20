// Deep-path imports from monaco-editor. The package's `exports` field uses a
// catch-all `"./*": "./*"` which Vite handles fine but TypeScript's module
// resolver needs an explicit declaration for type checking.

declare module 'monaco-editor/esm/vs/editor/editor.api.js' {
  export * from 'monaco-editor'
}

declare module 'monaco-editor/esm/vs/language/json/monaco.contribution' {
  const _side_effects: void
  export default _side_effects
}

declare module 'monaco-editor/esm/vs/editor/editor.worker?worker' {
  const WorkerClass: { new (): Worker }
  export default WorkerClass
}

declare module 'monaco-editor/esm/vs/language/json/json.worker?worker' {
  const WorkerClass: { new (): Worker }
  export default WorkerClass
}
