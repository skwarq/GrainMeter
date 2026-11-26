import { useEffect, useState } from 'preact/hooks';
import { LOG_LEVELS, logText } from '../lib/logs.js';

export function Logs({ model, lang, preview = false, describeError = (e) => e.message }) {
  const [, refresh] = useState(0);
  const [levels, setLevels] = useState(LOG_LEVELS);
  useEffect(() => model.subscribe(() => refresh((n) => n + 1)), [model]);
  const t = (key) => logText(lang, key);
  const text = model.text(levels);
  const download = () => {
    const url = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
    const link = document.createElement('a');
    link.href = url;
    link.download = 'grainmeter-logs.txt';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  };
  return (
    <details class="card remote-logs" onToggle={preview ? (e) => model.setOpen(e.currentTarget.open) : undefined}>
      <summary>{t('title')}</summary>
      <p>{t('hint')}</p>
      {preview && (
        <p>
          <a href="#/config">{t('configHint')}</a>
        </p>
      )}
      {preview && model.lost > 0 && (
        <p role="status">
          {t('lost')} {model.lost}
        </p>
      )}
      {preview && model.error && (
        <p role="alert">
          {t('pollingError')} {describeError(model.error)}
        </p>
      )}
      {['denied', 'error', 'subscribing', 'offline'].includes(model.state) && (
        <p role={['denied', 'error'].includes(model.state) ? 'alert' : 'status'}>{t(model.state)}</p>
      )}
      <div class="row-actions">
        <fieldset>
          <legend>{t('levels')}</legend>
          {LOG_LEVELS.map((level) => (
            <label key={level}>
              <input
                type="checkbox"
                checked={levels.includes(level)}
                onChange={(e) =>
                  setLevels(e.currentTarget.checked ? [...levels, level] : levels.filter((v) => v !== level))
                }
              />
              {level}
            </label>
          ))}
        </fieldset>
        <button type="button" onClick={() => model.pause(!model.paused)}>
          {t(model.paused ? 'resume' : 'pause')}
        </button>
        <button type="button" onClick={() => model.clear()}>
          {t('clear')}
        </button>
        <button type="button" onClick={download} disabled={!text}>
          {t('download')}
        </button>
      </div>
      {model.paused && <p role="status">{t(preview ? 'previewPaused' : 'paused')}</p>}
      <pre tabIndex="0" aria-label={t('title')}>
        {text || t('empty')}
      </pre>
    </details>
  );
}
