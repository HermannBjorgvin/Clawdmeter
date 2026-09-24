// Settings values, read through the global the JerryScript runtime provides.
// Not in @busy-app/busy-lib yet.

/** Stored values: a flat field_id → value map. */
export type SettingsValues = Record<string, unknown>;

interface SettingsApi {
  load(): Promise<{ values?: SettingsValues } | SettingsValues>;
  save(values: SettingsValues): Promise<void>;
}

function api(): SettingsApi | undefined {
  return (globalThis as { Settings?: SettingsApi }).Settings;
}

/** Reads the stored values, or null if unavailable. */
export async function loadValues(): Promise<SettingsValues | null> {
  const settings = api();
  if (!settings) return null;
  try {
    const result = await settings.load();
    // The runtime may return the whole config file or just the values.
    const values = (result as { values?: SettingsValues }).values ?? result;
    return values as SettingsValues;
  } catch {
    return null;
  }
}

/** Saves the values, if the runtime supports it. */
export async function saveValues(values: SettingsValues): Promise<void> {
  await api()?.save(values);
}
