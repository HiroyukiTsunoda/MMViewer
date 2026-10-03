import mermaid from 'mermaid';
import elkLayouts from '@mermaid-js/layout-elk';
import zenuml from '@mermaid-js/mermaid-zenuml';

mermaid.registerLayoutLoaders(elkLayouts);
const send = (text) => window.chrome.webview.postMessage(text);
let current = '', svg;
async function receive(text) {
  const [kind, id, theme, ...parts] = text.split('\n');
  if (kind === 'render') {
    current = id;
    try {
      document.body.style.background = theme === 'dark' ? '#1d222b' : '#f5f7fa';
      document.getElementById('diagram').replaceChildren();
      mermaid.initialize({startOnLoad: false, securityLevel: 'strict', theme,
        fontFamily: '"Yu Gothic UI", "Meiryo", sans-serif',
        maxTextSize: 1000000, maxEdges: 5000, suppressErrorRendering: true,
        secure: ['secure', 'securityLevel', 'startOnLoad', 'maxTextSize', 'maxEdges', 'suppressErrorRendering']});
      const rendered = await mermaid.render('mmDiagram', parts.join('\n'));
      const container = document.getElementById('diagram');
      container.innerHTML = rendered.svg;
      svg = container.querySelector('svg');
      if (!svg) throw new Error('Mermaid did not return an SVG.');
      await document.fonts.ready;
      const box = svg.viewBox.baseVal;
      const rect = svg.getBoundingClientRect();
      const width = Math.ceil(box.width || rect.width);
      const height = Math.ceil(box.height || rect.height);
      if (!(width > 0 && height > 0 && width <= 100000 && height <= 100000)) throw new Error('図のサイズが上限を超えています。');
      send(`size\n${id}\n${width}\n${height}`);
    } catch (error) { send(`error\n${id}\n${String(error.message || error)}`); }
  } else if (kind === 'capture' && current === id && svg) {
    svg.removeAttribute('style');
    svg.style.cssText = `display:block;max-width:none;width:${theme}px;height:${parts[0]}px`;
    svg.setAttribute('width', theme);svg.setAttribute('height', parts[0]);
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    send(`capture\n${id}`);
  }
}
(async () => {
  try {
    await mermaid.registerExternalDiagrams([zenuml]);
    window.chrome.webview.addEventListener('message', event => { receive(event.data).catch(error => send(`error\n${current}\n${error.message}`)); });
    send('ready');
  } catch (error) { send(`fatal\n${error.message}`); }
})();
