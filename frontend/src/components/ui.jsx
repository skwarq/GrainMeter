// Small presentational building blocks. All device-provided strings are
// rendered as text children (Preact escapes them); no innerHTML anywhere.

const ICONS = {
  overview: 'M5 21V9l7-5 7 5v12M5 13h14M9 21v-4h6v4',
  sensors: 'M10 13.5V5a2 2 0 1 1 4 0v8.5a4 4 0 1 1-4 0zM12 10v6',
  config: 'M4 7h9M17 7h3M4 17h3M11 17h9M15 5v4M9 15v4',
  upload: 'M7 18a4 4 0 0 1-.6-7.96A6 6 0 0 1 18 9a4.5 4.5 0 0 1 0 9M12 12v8M9 15l3-3 3 3',
  update: 'M8 4h8v4H8zM6 8h12v12H6zM12 11v6M9.5 14.5 12 17l2.5-2.5',
  diag: 'M3 12h4l2-5 4 10 2-5h6',
  sim: 'M9 3h6M10 3v6l-5 9a2 2 0 0 0 1.8 3h10.4a2 2 0 0 0 1.8-3l-5-9V3M7.5 14h9',
  service: 'M12 3v8M6.3 6.8a8 8 0 1 0 11.4 0',
  refresh: 'M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7',
  warn: 'M12 4 2.5 20h19zM12 10v4.5M12 17.2v.3',
  ok: 'M5 12.5 10 17l9-10',
  x: 'M6 6l12 12M18 6 6 18',
  lock: 'M7 11V8a5 5 0 0 1 10 0v3M5 11h14v10H5zM12 15v2',
  battery: 'M3 8h15v8H3zM18 10.5h2.5v3H18',
  wifi: 'M2.5 9a14 14 0 0 1 19 0M5.5 12.5a9.5 9.5 0 0 1 13 0M8.6 15.8a5 5 0 0 1 6.8 0M12 19.3v.2',
  clock: 'M12 7v5l3 2M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18',
  chip: 'M7 7h10v10H7zM10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4',
  db: 'M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3zM4 6v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3',
  copy: 'M8 8h12v12H8zM16 8V4H4v12h4',
  download: 'M12 4v11M7 10l5 5 5-5M4 20h16',
  search: 'M10.5 4a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13M15.5 15.5 20 20',
  link: 'M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1',
  globe:
    'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18M3 12h18M12 3c2.5 2.5 3.5 6 3.5 9s-1 6.5-3.5 9c-2.5-2.5-3.5-6-3.5-9s1-6.5 3.5-9',
};

export function Icon({ name, size = 20, class: cls }) {
  return (
    <svg
      class={'icon' + (cls ? ' ' + cls : '')}
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="1.8"
      stroke-linecap="round"
      stroke-linejoin="round"
      aria-hidden="true"
    >
      <path d={ICONS[name] || ICONS.warn} />
    </svg>
  );
}

/** Product mark: a silo silhouette whose probe ends in a thermometer bulb. */
export function Logo({ size = 30 }) {
  return (
    <svg class="logo" width={size} height={size} viewBox="0 0 32 32" aria-hidden="true">
      <rect x="1" y="1" width="30" height="30" rx="8" fill="var(--accent)" />
      <path d="M8 13 16 6l8 7v12H8z" fill="none" stroke="var(--on-accent)" stroke-width="2" stroke-linejoin="round" />
      <path d="M9 19c2.5-1.6 4.5-1.6 7 0s4.5 1.6 7 0v6H9z" fill="var(--on-accent)" opacity=".45" />
      <path d="M16 9v10" stroke="var(--on-accent)" stroke-width="1.6" stroke-linecap="round" />
      <circle cx="16" cy="20.5" r="2.2" fill="var(--on-accent)" />
    </svg>
  );
}

export function Card({ title, icon, actions, children, class: cls, tone }) {
  return (
    <section class={'card' + (cls ? ' ' + cls : '') + (tone ? ' tone-' + tone : '')}>
      {(title || actions) && (
        <header class="card-head">
          {icon && <Icon name={icon} size={18} />}
          {title && <h2>{title}</h2>}
          {actions && <div class="card-actions">{actions}</div>}
        </header>
      )}
      {children}
    </section>
  );
}

export function Button({ busy, children, icon, variant = 'secondary', small, ...rest }) {
  return (
    <button
      type="button"
      class={'btn btn-' + variant + (small ? ' btn-sm' : '') + (busy ? ' is-busy' : '')}
      disabled={busy || rest.disabled}
      {...rest}
    >
      {busy ? <span class="spinner" aria-hidden="true" /> : icon ? <Icon name={icon} size={16} /> : null}
      <span>{children}</span>
    </button>
  );
}

export function Badge({ tone = 'neutral', children, title }) {
  return (
    <span class={'badge badge-' + tone} title={title}>
      {children}
    </span>
  );
}

/** Result line of an action (from useAction). */
export function Result({ result }) {
  if (!result) return null;
  return (
    <p class={'result result-' + result.type} role={result.type === 'error' ? 'alert' : 'status'}>
      <Icon name={result.type === 'error' ? 'warn' : 'ok'} size={16} />
      <span>{result.text}</span>
    </p>
  );
}

export function KV({ items }) {
  return (
    <dl class="kv">
      {items
        .filter(Boolean)
        .map(([k, v]) => [
          <dt key={'k' + k}>{k}</dt>,
          <dd key={'v' + k}>{v === null || v === undefined ? '—' : v}</dd>,
        ])}
    </dl>
  );
}

/** Horizontal gauge 0..1 with optional tone. */
export function Meter({ value, tone = 'accent', label }) {
  const v = Math.max(0, Math.min(1, Number.isFinite(value) ? value : 0));
  return (
    <div
      class="meter"
      role="meter"
      aria-valuemin="0"
      aria-valuemax="100"
      aria-valuenow={Math.round(v * 100)}
      aria-label={label}
    >
      <div class={'meter-fill tone-bg-' + tone} style={{ width: (v * 100).toFixed(1) + '%' }} />
    </div>
  );
}

export function Loading({ text }) {
  return (
    <p class="loading">
      <span class="spinner" aria-hidden="true" /> {text}
    </p>
  );
}

export function Empty({ children }) {
  return <p class="empty">{children}</p>;
}

export function PageHead({ title, sub, actions }) {
  return (
    <div class="page-head">
      <div>
        <h1>{title}</h1>
        {sub && <p class="sub">{sub}</p>}
      </div>
      {actions && <div class="page-actions">{actions}</div>}
    </div>
  );
}
