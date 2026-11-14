import { readStoredLocalePref, resolveLocale, SUPPORTED_LOCALES } from './locale.js';
import zhCN from './locales/zh-CN.js';
import en from './locales/en.js';

export * from './locale.js';

const CATALOGS = { 'zh-CN': zhCN, en };

let activeLocale = resolveLocale(readStoredLocalePref());

const pluralCache = new Map();
const numberFmtCache = new Map();
const listFmtCache = new Map();
const dateFmtCache = new Map();

function getPluralRules(locale) {
  let pr = pluralCache.get(locale);
  if (!pr) {
    pr = new Intl.PluralRules(locale);
    pluralCache.set(locale, pr);
  }
  return pr;
}

export function getLocale() {
  return activeLocale;
}

export function setActiveLocale(locale) {
  activeLocale = SUPPORTED_LOCALES.includes(locale) ? locale : 'en';
  return activeLocale;
}

export function formatNumber(value, options, locale = activeLocale) {
  if (!Number.isFinite(value)) return '—';
  const key = `${locale}|${options ? JSON.stringify(options) : ''}`;
  let fmt = numberFmtCache.get(key);
  if (!fmt) {
    fmt = new Intl.NumberFormat(locale, options);
    numberFmtCache.set(key, fmt);
  }
  return fmt.format(value);
}

export function formatPercent(ratioOrValue, { isRatio = false, maximumFractionDigits = 0 } = {}, locale = activeLocale) {
  if (!Number.isFinite(ratioOrValue)) return '—';
  const ratio = isRatio ? ratioOrValue : ratioOrValue / 100;
  return formatNumber(ratio, { style: 'percent', maximumFractionDigits }, locale);
}

export function formatList(items, options = { style: 'short', type: 'conjunction' }, locale = activeLocale) {
  const list = items.filter(Boolean).map(String);
  if (!list.length) return '';
  if (list.length === 1) return list[0];
  const key = `${locale}|${options.style ?? ''}|${options.type ?? ''}`;
  let fmt = listFmtCache.get(key);
  if (!fmt) {
    fmt = new Intl.ListFormat(locale, options);
    listFmtCache.set(key, fmt);
  }
  return fmt.format(list);
}

export function formatDateTime(value, options, locale = activeLocale) {
  const date = value instanceof Date ? value : new Date(value);
  if (Number.isNaN(date.getTime())) return '—';
  const key = `${locale}|${options ? JSON.stringify(options) : ''}`;
  let fmt = dateFmtCache.get(key);
  if (!fmt) {
    fmt = new Intl.DateTimeFormat(locale, options);
    dateFmtCache.set(key, fmt);
  }
  return fmt.format(date);
}

export function pluralSelect(count, locale = activeLocale) {
  return getPluralRules(locale).select(Number(count) || 0);
}

function lookup(dict, key) {
  const parts = key.split('.');
  let cur = dict;
  for (const part of parts) {
    if (cur == null || typeof cur !== 'object') return undefined;
    cur = cur[part];
  }
  return cur;
}

function parsePluralForms(body) {
  const forms = {};
  let i = 0;
  while (i < body.length) {
    while (i < body.length && /\s/.test(body[i])) i += 1;
    if (i >= body.length) break;
    const keyStart = i;
    while (i < body.length && !/[\s{]/.test(body[i])) i += 1;
    const formKey = body.slice(keyStart, i);
    while (i < body.length && body[i] !== '{') i += 1;
    if (body[i] !== '{') break;
    i += 1;
    const valStart = i;
    let depth = 1;
    while (i < body.length && depth > 0) {
      if (body[i] === '{') depth += 1;
      else if (body[i] === '}') depth -= 1;
      if (depth > 0) i += 1;
    }
    forms[formKey] = body.slice(valStart, i);
    i += 1;
  }
  return forms;
}

function interpolate(template, params, locale) {
  let out = '';
  let i = 0;
  while (i < template.length) {
    const open = template.indexOf('{', i);
    if (open < 0) {
      out += template.slice(i);
      break;
    }
    out += template.slice(i, open);
    let depth = 1;
    let j = open + 1;
    while (j < template.length && depth > 0) {
      if (template[j] === '{') depth += 1;
      else if (template[j] === '}') depth -= 1;
      if (depth > 0) j += 1;
    }
    if (depth !== 0) {
      out += template.slice(open);
      break;
    }
    const expr = template.slice(open + 1, j);
    i = j + 1;

    const firstComma = expr.indexOf(',');
    if (firstComma < 0) {
      const name = expr.trim();
      const val = params?.[name];
      out += val == null ? '' : (typeof val === 'number' ? formatNumber(val, undefined, locale) : String(val));
      continue;
    }

    const varName = expr.slice(0, firstComma).trim();
    const rest = expr.slice(firstComma + 1).trim();
    const secondComma = rest.indexOf(',');
    const kind = (secondComma >= 0 ? rest.slice(0, secondComma) : rest).trim();
    if (kind === 'plural' && secondComma >= 0) {
      const rawCount = Number(params?.[varName] ?? 0);
      const forms = parsePluralForms(rest.slice(secondComma + 1));
      const exactKey = `=${rawCount}`;
      const category = pluralSelect(rawCount, locale);
      const chosen = forms[exactKey] ?? forms[category] ?? forms.other ?? '';
      const formattedCount = formatNumber(rawCount, undefined, locale);
      const expanded = chosen.replace(/#/g, formattedCount);
      out += interpolate(expanded, params, locale);
    } else {
      const val = params?.[varName];
      out += val == null ? '' : String(val);
    }
  }
  return out;
}

export function t(key, params, locale = activeLocale) {
  const dict = CATALOGS[locale] ?? CATALOGS.en;
  let entry = lookup(dict, key);
  if (entry === undefined && locale !== 'en') {
    entry = lookup(CATALOGS.en, key);
  }
  if (entry === undefined) return key;
  if (typeof entry === 'function') return entry(params ?? {}, locale);
  if (typeof entry !== 'string') return String(entry);
  return params ? interpolate(entry, params, locale) : entry;
}
