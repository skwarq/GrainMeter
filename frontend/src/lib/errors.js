// Maps API failures to message keys (see src/i18n/*.js, section "err.*").
// The server text (if any) is passed through as `detail`; it is rendered as a
// text node, never as HTML.

export class ApiError extends Error {
  /**
   * @param {'network'|'timeout'|'http'|'parse'|'aborted'} kind
   * @param {number} status HTTP status (0 when no response)
   * @param {string} [serverMessage] "error" text from the device
   */
  constructor(kind, status = 0, serverMessage = '') {
    super(serverMessage || kind + (status ? ' ' + status : ''));
    this.name = 'ApiError';
    this.kind = kind;
    this.status = status;
    this.serverMessage = serverMessage;
  }
}

const HTTP_KEYS = {
  400: 'err.400',
  401: 'err.401',
  404: 'err.404',
  409: 'err.409',
  411: 'err.411',
  413: 'err.413',
  415: 'err.415',
  422: 'err.422',
  500: 'err.500',
};

const OTA_KEYS = {
  413: 'err.ota413',
  415: 'err.ota415',
  422: 'err.ota422',
  500: 'err.ota500',
};

/**
 * @param {unknown} err
 * @param {'ota'|undefined} [context]
 * @returns {{key:string, params:object, detail:string}}
 */
export function describeError(err, context) {
  if (!err || typeof err !== 'object') return { key: 'err.unknown', params: {}, detail: '' };
  const e = /** @type {any} */ (err);
  const detail = typeof e.serverMessage === 'string' ? e.serverMessage : '';
  switch (e.kind) {
    case 'network':
      return { key: 'err.network', params: {}, detail: '' };
    case 'timeout':
      return { key: 'err.timeout', params: {}, detail: '' };
    case 'parse':
      return { key: 'err.parse', params: {}, detail: '' };
    case 'aborted':
      return { key: 'err.aborted', params: {}, detail: '' };
    case 'http': {
      const s = e.status;
      const key = (context === 'ota' && OTA_KEYS[s]) || HTTP_KEYS[s] || 'err.http';
      return { key, params: { status: s }, detail };
    }
    default:
      return { key: 'err.unknown', params: {}, detail: e.message ? String(e.message) : '' };
  }
}

/** True when the failure means "the device does not answer" (restart / out of range). */
export function isOffline(err) {
  return !!err && (err.kind === 'network' || err.kind === 'timeout');
}
