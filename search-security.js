import { fetch } from 'undici';

export async function fetchPublicText(url, options = {}) {
  try {
    const parsed = new URL(url);
    if (!['http:', 'https:'].includes(parsed.protocol)) {
      return null;
    }
    const res = await fetch(url, {
      ...options,
      signal: AbortSignal.timeout(options.timeout || 10000),
    });
    if (!res.ok) return null;
    return await res.text();
  } catch {
    return null;
  }
}
