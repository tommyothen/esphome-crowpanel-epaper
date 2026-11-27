// Copies the repository's README and docs/ into the site's content, so the docs live next to the
// code and still read as plain Markdown on GitHub. Each file's first heading becomes its page
// title, links between them become links between pages, and links to anything else in the
// repository go to it on GitHub. Run before every dev server and build (see package.json).
import { mkdir, readdir, readFile, rm, writeFile } from 'node:fs/promises';
import { dirname, join, posix } from 'node:path';

const REPO = 'https://github.com/tommyothen/esphome-crowpanel-epaper';
const SITE = 'https://tommyothen.github.io/esphome-crowpanel-epaper/';
const root = join(import.meta.dirname, '..', '..');
const out = join(import.meta.dirname, '..', 'src', 'content', 'docs');

// A source file, relative to the repository's root, and its page's slug ('' for the home page).
type Page = { source: string; slug: string };

const docs = (await readdir(join(root, 'docs'))).filter((file) => file.endsWith('.md'));
const pages: Page[] = [
  { source: 'README.md', slug: '' },
  ...docs.map((file) => ({ source: `docs/${file}`, slug: file.slice(0, -'.md'.length) })),
];
const slugs = new Map(pages.map((page) => [page.source, page.slug]));

// Where a link in a page's Markdown should go on the site.
function linkFrom(page: Page, target: string): string {
  if (/^[a-z][a-z+.-]*:/i.test(target) || target.startsWith('#')) return target;
  const [path, anchor] = target.split('#');
  const hash = anchor ? `#${anchor}` : '';
  const repoPath = posix.normalize(posix.join(posix.dirname(page.source), path));
  const slug = slugs.get(repoPath);
  if (slug !== undefined) {
    const up = page.slug === '' ? '' : '../';
    return `${up}${slug === '' ? '' : `${slug}/`}${hash}` || './';
  }
  const kind = path.endsWith('/') ? 'tree' : 'blob';
  return `${REPO}/${kind}/main/${repoPath}${path.endsWith('/') ? '/' : ''}${hash}`;
}

await rm(out, { recursive: true, force: true });
await mkdir(out, { recursive: true });
for (const page of pages) {
  const markdown = await readFile(join(root, page.source), 'utf8');
  const heading = markdown.match(/^# (.+)\n+/);
  if (!heading) throw new Error(`${page.source} has no title heading`);
  const body = markdown
    .slice(heading[0].length)
    // A paragraph pointing readers at this site belongs on GitHub, not on the site.
    .split(/\n{2,}/)
    .filter((paragraph) => !paragraph.includes(SITE))
    .join('\n\n')
    .replace(/\]\(([^)\s]+)\)/g, (_, target: string) => `](${linkFrom(page, target)})`);
  const frontmatter = [
    `title: ${JSON.stringify(heading[1])}`,
    `editUrl: ${JSON.stringify(`${REPO}/edit/main/${page.source}`)}`,
  ];
  const file = join(out, `${page.slug || 'index'}.md`);
  await mkdir(dirname(file), { recursive: true });
  await writeFile(file, `---\n${frontmatter.join('\n')}\n---\n\n${body}`);
}
console.log(`Synced ${pages.length} pages from the README and docs/.`);
