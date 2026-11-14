// BCP-47 locale negotiation and preference persistence (`auto` | `zh-CN` | `en`).

export const SUPPORTED_LOCALES = ['zh-CN', 'en'];
export const LOCALE_PREFS = ['auto', 'zh-CN', 'en'];
export const LOCALE_STORAGE_KEY = 'upix.web.locale';

/**
 * Negotiate a single BCP-47 language tag to one of `zh-CN`, `en`, or `null`
 * (unrecognized language, keep scanning `navigator.languages`).
 *
 * Maps `zh-Hans`, `zh-CN`, `zh-SG`, `zh` (without Traditional script `Hant`
 * or regions `TW`/`HK`/`MO`) -> `zh-CN`; maps Traditional Chinese and `en*`
 * -> `en`.
 */
export function matchSupportedLocale(tag) {
  if (!tag || typeof tag !== 'string') return null;
  const trimmed = tag.trim();
  if (!trimmed) return null;
  try {
    const loc = new Intl.Locale(trimmed);
    const lang = (loc.language || '').toLowerCase();
    const script = loc.script
      ? `${loc.script[0].toUpperCase()}${loc.script.slice(1).toLowerCase()}`
      : '';
    const region = (loc.region || '').toUpperCase();

    if (lang === 'zh' || lang === 'cmn') {
      if (script === 'Hans') return 'zh-CN';
      if (script === 'Hant' || region === 'TW' || region === 'HK' || region === 'MO') {
        return 'en';
      }
      if (region === 'CN' || region === 'SG' || (!script && !region)) {
        return 'zh-CN';
      }
      return 'en';
    }
    if (lang === 'en') {
      return 'en';
    }
    return null;
  } catch {
    return null;
  }
}

export function detectBrowserLocale(nav = typeof navigator !== 'undefined' ? navigator : null) {
  const candidates = Array.isArray(nav?.languages) && nav.languages.length
    ? nav.languages
    : (nav?.language ? [nav.language] : []);

  for (const tag of candidates) {
    const matched = matchSupportedLocale(tag);
    if (matched) return matched;
  }
  return 'en';
}

export function normalizeLocalePref(pref) {
  if (typeof pref !== 'string') return 'auto';
  const lower = pref.trim().toLowerCase();
  if (lower === 'zh-cn') return 'zh-CN';
  if (lower === 'en') return 'en';
  return 'auto';
}

export function resolveLocale(pref, nav) {
  const normalized = normalizeLocalePref(pref);
  return normalized === 'auto' ? detectBrowserLocale(nav) : normalized;
}

export function readStoredLocalePref() {
  try {
    if (typeof localStorage === 'undefined') return 'auto';
    return normalizeLocalePref(localStorage.getItem(LOCALE_STORAGE_KEY));
  } catch {
    return 'auto';
  }
}

export function writeStoredLocalePref(pref) {
  try {
    if (typeof localStorage === 'undefined') return;
    const normalized = normalizeLocalePref(pref);
    if (normalized === 'auto') localStorage.removeItem(LOCALE_STORAGE_KEY);
    else localStorage.setItem(LOCALE_STORAGE_KEY, normalized);
  } catch {
    /* storage blocked */
  }
}
