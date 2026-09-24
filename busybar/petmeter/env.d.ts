/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** BUSY Bar address for dev mode (see .env). */
  readonly VITE_BUSY_ADDR?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}

/** A .anim file is a binary device asset; Vite returns its URL. */
declare module '*.anim' {
  const src: string
  export default src
}
