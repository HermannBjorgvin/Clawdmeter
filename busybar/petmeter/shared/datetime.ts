const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

const pad = (n: number) => String(n).padStart(2, "0");

/** Time as "HH:MM". */
export function formatHM(d: Date): string {
  return `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** Three-letter month, e.g. "Jul". */
export function formatMonth(d: Date): string {
  return MONTHS[d.getMonth()];
}

/** Three-letter weekday, e.g. "Sat". */
export function formatWeekday(d: Date): string {
  return DAYS[d.getDay()];
}

/** Day of the month, e.g. "8". */
export function formatDay(d: Date): string {
  return String(d.getDate());
}
