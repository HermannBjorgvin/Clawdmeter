import { BusyBar } from "@busy-app/busy-lib";

/** Device address; in dev from VITE_BUSY_ADDR (.env). */
const addr = import.meta.env.VITE_BUSY_ADDR ?? "localhost";

/** Shared device client. */
export const device = new BusyBar({ addr });
