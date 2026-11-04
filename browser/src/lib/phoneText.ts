/* Text helpers for the Phone window. */

/** When a call in the log started: the time today, the weekday and time this
 *  week, else the date; '' when the device's clock was unset. */
export function callTime(at: number, now = Date.now()): string {
  if (!(at > 0)) return ''
  const d = new Date(at * 1000)
  const today = new Date(now)
  const time = d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' })
  if (d.toDateString() === today.toDateString()) return time
  const days = (new Date(today.toDateString()).getTime() - new Date(d.toDateString()).getTime()) / 86400000
  if (days > 0 && days < 7) return `${d.toLocaleDateString(undefined, { weekday: 'short' })} ${time}`
  return d.toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' })
}

/** A 32-hex hash in groups of four, for reading aloud or by eye. */
export function groupHash(h: string): string {
  return h.replace(/(.{4})(?=.)/g, '$1 ')
}
