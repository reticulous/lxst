/**
 * callNotice — an incoming call as a system notification, for a page that is
 * not in front: a background tab, a covered window, or a page not yet clicked
 * since it loaded (whose own ring the browser holds back). The operating
 * system sounds it, and clicking it brings this tab forward.
 *
 * Permission is asked from a gesture (askCallNotices), once: a browser shows
 * its prompt only for one, and an answer either way is final. Without
 * permission, or outside a secure context, nothing is shown.
 */

const TAG = 'lxst-call'

let shown: Notification | null = null

function supported(): boolean {
  return typeof Notification !== 'undefined' && window.isSecureContext
}

/** Ask for permission if it has not been asked. Call from a user gesture. */
export function askCallNotices() {
  if (!supported() || Notification.permission !== 'default') return
  Notification.requestPermission().catch(() => { /* asked again next gesture */ })
}

/** Show the notice for a ringing call, unless the page is in front and
 *  already shows it. `onOpen` runs when the notice is clicked. */
export function showCallNotice(caller: string, onOpen: () => void) {
  if (!supported() || Notification.permission !== 'granted') return
  if (document.visibilityState === 'visible' && document.hasFocus()) return
  hideCallNotice()
  try {
    const n = new Notification('Incoming call', {
      body: caller, tag: TAG, requireInteraction: true,
    })
    n.onclick = () => { window.focus(); onOpen(); n.close() }
    shown = n
  } catch (e) {
    console.warn('[lxst] call notice:', e)
  }
}

export function hideCallNotice() {
  if (shown) { shown.close(); shown = null }
}
