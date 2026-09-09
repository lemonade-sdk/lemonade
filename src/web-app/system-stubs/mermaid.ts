function escapeXml(value: string): string {
  return value
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

const mermaid = {
  initialize() {
    // No-op in distro system-node builds: mermaid is not available as a
    // packaged system dependency across the supported runners.
  },

  async render(id: string, source: string): Promise<{ svg: string }> {
    const label = escapeXml(source.split('\n').slice(0, 6).join('\n'));
    return {
      svg: `<svg id="${escapeXml(id)}" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 120" role="img" aria-label="Mermaid diagram fallback"><rect width="640" height="120" rx="8" fill="none" stroke="currentColor" opacity="0.25"/><text x="16" y="32" fill="currentColor" font-size="14">Mermaid diagram unavailable in system package build.</text><text x="16" y="60" fill="currentColor" font-size="12">${label}</text></svg>`,
    };
  },
};

export default mermaid;
