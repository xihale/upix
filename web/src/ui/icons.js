// Minimalist geometric icons: 24px grid, 1.5px stroke, currentColor.
// Used sparingly where spatial symbols outperform words (+, ×, search, theme).

const PATHS = {
  upload: '<path d="M12 16V4"/><path d="M7 9l5-5 5 5"/><path d="M4 18h16"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  x: '<path d="M6 6l12 12M18 6L6 18"/>',
  download: '<path d="M12 4v12"/><path d="M7 11l5 5 5-5"/><path d="M4 20h16"/>',
  trash: '<path d="M4 7h16"/><path d="M9 7V4h6v3"/><path d="M6.5 7l1 13h9l1-13"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  'chevron-down': '<path d="M6 9l6 6 6-6"/>',
  'chevron-right': '<path d="M9 6l6 6-6 6"/>',
  search: '<circle cx="11" cy="11" r="6.5"/><path d="M16 16l4.5 4.5"/>',
  refresh: '<path d="M20 11a8 8 0 00-14-4.5L4 9"/><path d="M4 4v5h5"/><path d="M4 13a8 8 0 0014 4.5L20 15"/><path d="M20 20v-5h-5"/>',
  alert: '<circle cx="12" cy="12" r="9"/><path d="M12 8v5M12 16v.5"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8v.5"/>',
  copy: '<rect x="8" y="8" width="11" height="11" rx="1.5"/><path d="M16 8V5.5A1.5 1.5 0 0014.5 4h-9A1.5 1.5 0 004 5.5v9A1.5 1.5 0 005.5 16H8"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2.5v2.5M12 19v2.5M2.5 12H5M19 12h2.5M5.3 5.3l1.8 1.8M16.9 16.9l1.8 1.8M18.7 5.3l-1.8 1.8M7.1 16.9l-1.8 1.8"/>',
  moon: '<path d="M20 14.5A8 8 0 019.5 4a8 8 0 1010.5 10.5z"/>',
  contrast: '<circle cx="12" cy="12" r="8.5"/><path d="M12 3.5v17a8.5 8.5 0 000-17z" fill="currentColor" stroke="none"/>',
};

const template = document.createElement('template');

export function icon(name, size = 16, className = '') {
  const body = PATHS[name];
  if (!body) throw new Error(`unknown icon: ${name}`);
  template.innerHTML = `<svg class="icon ${className}" width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="square" stroke-linejoin="miter" aria-hidden="true" focusable="false">${body}</svg>`;
  return template.content.firstChild.cloneNode(true);
}

// Unified brand mark matching Android ic_launcher_foreground / ic_launcher_monochrome:
// 24dp glyph bounds [2, 22.75] x [1.25, 22] mapped full-bleed in var(--ink).
export function brandMark(size = 20) {
  template.innerHTML = `<svg class="brand-mark" width="${size}" height="${size}" viewBox="2 1.25 20.75 20.75" fill="var(--ink)" aria-hidden="true" focusable="false"><path d="M15.9062,2C15.9062,2.41421 16.242,2.75 16.6562,2.75H20.1893L12.75,10.1893V8C12.75,7.58579 12.4142,7.25 12,7.25C11.5858,7.25 11.25,7.58579 11.25,8V12C11.25,12.4142 11.5858,12.75 12,12.75H16C16.4142,12.75 16.75,12.4142 16.75,12C16.75,11.5858 16.4142,11.25 16,11.25H13.8107L21.25,3.81066V7.34375C21.25,7.75796 21.5858,8.09375 22,8.09375C22.4142,8.09375 22.75,7.75796 22.75,7.34375V2C22.75,1.58579 22.4142,1.25 22,1.25H16.6562C16.242,1.25 15.9062,1.58579 15.9062,2Z"/><path d="M19.7516,7.43034C19.797,8.62969 20.7812,9.5887 21.9904,9.59373C22,10.3198 22,11.1187 22,12C22,16.714 22,19.0711 20.5355,20.5355C19.0711,22 16.714,22 12,22C7.28595,22 4.92893,22 3.46447,20.5355C2,19.0711 2,16.714 2,12C2,7.28595 2,4.92893 3.46447,3.46447C4.92893,2 7.28595,2 12,2C12.8813,2 13.6802,2 14.4063,2.00957C14.4113,3.21884 15.3703,4.20298 16.5697,4.24836L13.9468,6.87122C13.5572,6.20078 12.8313,5.75 12,5.75C10.7574,5.75 9.75,6.75736 9.75,8V12C9.75,13.2426 10.7574,14.25 12,14.25H16C17.2426,14.25 18.25,13.2426 18.25,12C18.25,11.1687 17.7992,10.4428 17.1288,10.0532L19.7516,7.43034Z"/></svg>`;
  return template.content.firstChild.cloneNode(true);
}
