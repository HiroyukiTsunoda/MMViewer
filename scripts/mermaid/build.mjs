import { build } from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const here = path.dirname(fileURLToPath(import.meta.url));
const target = path.resolve(here, '../../vendor/mermaid');
fs.mkdirSync(target, {recursive: true});
await build({entryPoints: [path.join(here, 'renderer.js')], bundle: true, minify: true,
  format: 'iife', platform: 'browser', target: 'chrome120', legalComments: 'eof',
  outfile: path.join(target, 'renderer.js'), loader: {'.woff2':'dataurl', '.woff':'dataurl', '.ttf':'dataurl'}});
const cssPath = path.join(here, 'node_modules/katex/dist/katex.min.css');
const css = fs.readFileSync(cssPath, 'utf8').replace(/url\(([^)]+)\)/g, (_, name) => {
  const file = path.resolve(path.dirname(cssPath), name.replace(/["']/g,''));
  const mime = file.endsWith('.woff2') ? 'font/woff2' : file.endsWith('.woff') ? 'font/woff' : 'font/ttf';
  return `url(data:${mime};base64,${fs.readFileSync(file).toString('base64')})`;
});
fs.writeFileSync(path.join(target, 'index.html'), `<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src data:; font-src data:; connect-src 'none'; frame-src 'none'; base-uri 'none'">
<style>html,body{margin:0;padding:0;overflow:hidden}#diagram{width:max-content} ${css}</style>
</head><body><div id="diagram"></div><script src="/renderer.js"></script></body></html>`);
// Include the distribution licenses for every resolved package, not just Mermaid.
const notices = [];
function walkModules(dir) {
  for (const item of fs.readdirSync(dir, {withFileTypes:true})) {
    if (!item.isDirectory() || item.name.startsWith('.')) continue;
    const folder = path.join(dir,item.name);
    if (item.name.startsWith('@')) { walkModules(folder);continue; }
    const manifest = path.join(folder,'package.json');
    if (!fs.existsSync(manifest)) continue;
    const pkg = JSON.parse(fs.readFileSync(manifest,'utf8'));
    const files = fs.readdirSync(folder).filter(name=>/^(licen[sc]e|copying|notice)(\.|$)/i.test(name));
    notices.push(`\n===== ${pkg.name} ${pkg.version} (${pkg.license || 'see license'}) =====\n`);
    for (const file of files) if (fs.statSync(path.join(folder,file)).isFile()) notices.push(fs.readFileSync(path.join(folder,file),'utf8'));
    if (fs.existsSync(path.join(folder,'node_modules'))) walkModules(path.join(folder,'node_modules'));
  }
}
walkModules(path.join(here,'node_modules'));
fs.writeFileSync(path.join(target,'THIRD_PARTY_NOTICES.txt'),notices.join('\n'));
console.log('Generated offline Mermaid 11.17.2 + ELK + ZenUML assets:',target);
