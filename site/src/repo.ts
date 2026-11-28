// The repository on GitHub, and its latest release for the foot of the sidebar.

const SLUG = 'tommyothen/esphome-crowpanel-epaper';
export const REPO_URL = `https://github.com/${SLUG}`;

/** Asks GitHub's API for the latest release's tag. Undefined when the request fails, as
 *  unauthenticated requests are rate limited per IP. */
export async function fetchLatestVersion(): Promise<string | undefined> {
  try {
    const res = await fetch(`https://api.github.com/repos/${SLUG}/releases/latest`);
    const release: unknown = res.ok ? await res.json() : undefined;
    return typeof release === 'object' && release !== null && 'tag_name' in release && typeof release.tag_name === 'string'
      ? release.tag_name
      : undefined;
  } catch {
    return undefined;
  }
}

let shared: Promise<string | undefined> | undefined;
/** fetchLatestVersion, asked once and shared, as every page of the build renders the sidebar. */
export const sharedLatestVersion = () => (shared ??= fetchLatestVersion());
