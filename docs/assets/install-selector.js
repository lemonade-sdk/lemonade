// Lemonade Install Selector - Clean Rewrite
// Supports: Windows/Linux, App+Server/ServerOnly, OGA/llama.cpp/FastFlowLM, NPU/GPU/CPU

const ALLOWLIST = [
  // Windows: OGA (NPU, CPU), llama.cpp (CPU, GPU), FastFlowLM (NPU)
  { os: 'win', fw: 'oga', dev: 'npu' },
  { os: 'win', fw: 'oga', dev: 'cpu' },
  { os: 'win', fw: 'llama', dev: 'cpu' },
  { os: 'win', fw: 'llama', dev: 'gpu' },
  { os: 'win', fw: 'flm', dev: 'npu' },
  // Linux: llama.cpp only (CPU, GPU)
  { os: 'linux', fw: 'llama', dev: 'cpu' },
  { os: 'linux', fw: 'llama', dev: 'gpu' },
  // Docker: llama.cpp only (CPU, GPU)
  { os: 'docker', fw: 'llama', dev: 'cpu' },
  { os: 'docker', fw: 'llama', dev: 'gpu' },
];

const NPU_DRIVER_URL = 'https://account.amd.com/en/forms/downloads/ryzenai-eula-public-xef.html?filename=NPU_RAI1.5_280_WHQL.zip';
const GITHUB_RELEASES_API = 'https://api.github.com/repos/lemonade-sdk/lemonade/releases/latest';

// State: os, type, fw, dev, latestVersion
window.lmnState = { os: 'win', distro: 'win', type: 'app', fw: 'oga', dev: 'npu' };
window.lmnLatestVersion = null;

// Fetch latest version from GitHub
async function fetchLatestVersion() {
  try {
    const response = await fetch(GITHUB_RELEASES_API);
    const data = await response.json();
    // tag_name is like "v1.2.3" or "1.2.3"
    window.lmnLatestVersion = data.tag_name.replace(/^v/, '');
    lmnRender(); // Re-render with version
  } catch (e) {
    console.warn('Failed to fetch latest version:', e);
    window.lmnLatestVersion = 'VERSION'; // Fallback placeholder
  }
}

function isAllowed(os, fw, dev) {
  return ALLOWLIST.some(c => c.os === os && c.fw === fw && c.dev === dev);
}

function findValidCombo(os, fw, dev) {
  // Try exact match first
  if (isAllowed(os, fw, dev)) return { os, fw, dev };

  // Try keeping fw, find valid dev
  for (const d of ['npu', 'gpu', 'cpu']) {
    if (isAllowed(os, fw, d)) return { os, fw, dev: d };
  }

  // Try keeping dev, find valid fw
  for (const f of ['oga', 'llama', 'flm']) {
    if (isAllowed(os, f, dev)) return { os, fw: f, dev };
  }

  // Fall back to first valid combo for this OS
  const fallback = ALLOWLIST.find(c => c.os === os);
  return fallback || ALLOWLIST[0];
}

window.lmnSet = function(field, value) {
  const newState = { ...lmnState, [field]: value };

  // OS change may invalidate fw/dev combo
  if (field === 'os') {
    const valid = findValidCombo(value, newState.fw, newState.dev);
    newState.fw = valid.fw;
    newState.dev = valid.dev;
    if (value === 'linux') { newState.distro = 'ubuntu'; }
    else if (value === 'win') { newState.distro = 'win'; }
    else if (value === 'macos') { newState.distro = 'macos'}
  }
  // fw/dev change - validate
  else if (field === 'fw' || field === 'dev') {
    const valid = findValidCombo(newState.os, newState.fw, newState.dev);
    newState.fw = valid.fw;
    newState.dev = valid.dev;
  }

  window.lmnState = newState;
  lmnRender();
};

window.lmnRender = function() {
  const { os, type, fw, dev } = lmnState;

  // Update active states and disabled states
  const cells = {
    os: ['win', 'linux', 'macos', 'docker'],
    distro: ['win', 'macos', 'ubuntu', 'arch', 'fedora', 'debian'],
    type: ['app', 'server'],
    fw: ['oga', 'llama', 'flm'],
    dev: ['npu', 'gpu', 'cpu']
  };

  // Reset and set active
  Object.entries(cells).forEach(([category, options]) => {
    options.forEach(opt => {
      const el = document.getElementById(`${category}-${opt}`);
      if (!el) return;
      el.className = '';
      el.onclick = () => lmnSet(category, opt);
      if (lmnState[category] === opt) el.classList.add('lmn-active');
    });
  });

  // Gray out invalid combinations
  cells.fw.forEach(f => {
    const el = document.getElementById(`fw-${f}`);
    if (el && !isAllowed(os, f, dev) && !ALLOWLIST.some(c => c.os === os && c.fw === f)) {
      el.classList.add('lmn-disabled');
    }
  });

  cells.dev.forEach(d => {
    const el = document.getElementById(`dev-${d}`);
    if (el && !isAllowed(os, fw, d)) {
      el.classList.add('lmn-disabled');
    }
  });

  // Render download section
  renderDownload();

  // Render quick start commands
  renderQuickStart();
};

function renderDownload() {
  const { os, distro, type } = lmnState;
  const osDistro = document.getElementById('lmn-install-distro');
  const downloadArea = document.getElementById('lmn-download-area');
  const installType = document.getElementById('lmn-install-type');
  const cmdDiv = document.getElementById('lmn-command');
  const installCmdDiv = document.getElementById('lmn-install-commands');
  const version = window.lmnLatestVersion || 'VERSION';

  // Handle macOS "Coming soon"
  if (os === 'macos') {
    if (downloadArea) downloadArea.style.display = 'none';
    if (installType) installType.style.display = 'table-row';
    if (installCmdDiv) installCmdDiv.style.display = 'none';
    if (osDistro) osDistro.style.display = 'none';
    if (cmdDiv) {
      cmdDiv.innerHTML = `<div class="lmn-coming-soon">Coming soon!</div>`;
    }
    return;
  }

  if (os === 'win') {
    if (osDistro) osDistro.style.display = 'none';
    if (installType) installType.style.display = 'table-row';

    // Windows: Show download button
    let link, buttonText;
    if (type === 'app') {
      link = 'https://github.com/lemonade-sdk/lemonade/releases/latest/download/lemonade.msi';
      buttonText = 'Download Lemonade Installer (.msi)';
    } else {
      link = 'https://github.com/lemonade-sdk/lemonade/releases/latest/download/lemonade-server-minimal.msi';
      buttonText = 'Download Lemonade Minimal Installer (.msi)';
    }

    if (downloadArea) {
      downloadArea.style.display = 'block';
      const linkEl = document.getElementById('lmn-link');
      if (linkEl) {
        linkEl.href = link;
        linkEl.textContent = buttonText;
      }
    }
    if (installCmdDiv) {
      installCmdDiv.style.display = 'none';
    }
  }

  if (os === 'linux') {
    if (osDistro) osDistro.style.display = 'table-row';
    if (installType) installType.style.display = 'table-row';

    if (distro === 'ubuntu') {
      // Ubuntu: Show snap and deb package commands
      let snapCommands, debFile;
      if (type === 'app') {
        // For App + Server, need to install both snaps
        // Install server first, then the desktop app (which connects to the server)
        snapCommands = [
          'sudo snap install lemonade-server',
          'sudo snap connect lemonade-server:process-control',
          'sudo snap install lemonade'
        ];
        debFile = `lemonade_${version}_amd64.deb`;
      } else {
        // For Server Only, just need the server snap
        snapCommands = [
          'sudo snap install lemonade-server',
          'sudo snap connect lemonade-server:process-control'
        ];
        debFile = `lemonade-server-minimal_${version}_amd64.deb`;
      }
      const downloadUrl = `https://github.com/lemonade-sdk/lemonade/releases/latest/download/${debFile}`;

      if (downloadArea) {
        downloadArea.style.display = 'none';
      }
      if (installCmdDiv) {
        installCmdDiv.style.display = 'block';
        const commands = [
          `wget ${downloadUrl}`,
          `sudo dpkg -i ${debFile}`
        ];

        installCmdDiv.innerHTML = `
          <div class="lmn-install-method-header">Install via Debian package:</div>
          <pre><code class="language-bash" id="lmn-install-pre-block"></code></pre>
          <div class="lmn-install-method-header">Or install via Snap (for a fully sandboxed portable experience):</div>
          <pre><code class="language-bash" id="lmn-install-snap-block"></code></pre>
        `;

        setTimeout(() => {
          // Render deb commands
          const pre = document.getElementById('lmn-install-pre-block');
          if (pre) {
            pre.innerHTML = commands.map((line, idx) => {
              const safeLine = line.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
              return `<div class="lmn-command-line"><span>${safeLine}</span><button class="lmn-copy-btn" title="Copy" onclick="lmnCopyInstallLine(event, ${idx})">📋</button></div>`;
            }).join('');
          }

          // Render snap command(s)
          const snapPre = document.getElementById('lmn-install-snap-block');
          if (snapPre) {
            snapPre.innerHTML = snapCommands.map((cmd, idx) => {
              const safeLine = cmd.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
              return `<div class="lmn-command-line"><span>${safeLine}</span><button class="lmn-copy-btn" title="Copy" onclick="lmnCopySnapLine(event, ${idx})">📋</button></div>`;
            }).join('');
          }
        }, 0);
      }
    } else if (distro === 'arch') {
      // Arch Linux: Show download button
      let link, buttonText;
      if (type === 'app') {
        link = 'https://aur.archlinux.org/packages/lemonade-desktop';
        buttonText = 'Download Lemonade Desktop (AUR)';
      } else {
        link = 'https://aur.archlinux.org/packages/lemonade-server';
        buttonText = 'Download Lemonade Server (AUR)';
      }

      if (downloadArea) {
        downloadArea.style.display = 'block';
        const linkEl = document.getElementById('lmn-link');
        if (linkEl) {
          linkEl.href = link;
          linkEl.target = '_blank';
          linkEl.textContent = buttonText;
        }
      }
      if (installCmdDiv) {
        installCmdDiv.style.display = 'none';
      }
    } else if (distro === 'fedora') {
      if (downloadArea) downloadArea.style.display = 'none';
      if (installCmdDiv) installCmdDiv.style.display = 'none';
      if (cmdDiv) {
        cmdDiv.innerHTML = `<div class="lmn-coming-soon">For Fedora, please follow the build instructions as described in the <a href="https://github.com/lemonade-sdk/lemonade/blob/main/docs/dev-getting-started.md#building-from-source" target="_blank">Developer Guide</a>.</div>`;
      }
      return;
    } else if (distro === 'debian') {
      if (downloadArea) downloadArea.style.display = 'none';
      if (installCmdDiv) installCmdDiv.style.display = 'none';
      if (cmdDiv) {
        cmdDiv.innerHTML = `<div class="lmn-coming-soon">For Debian, please follow the build instructions as described in the <a href="https://github.com/lemonade-sdk/lemonade/blob/main/docs/dev-getting-started.md#building-from-source" target="_blank">Developer Guide</a>.</div>`;
      }
      return;
    }
  }

  if (os === 'docker') {
    if (osDistro) osDistro.style.display = 'none';

    if (installType) installType.style.display = 'none';

    if (downloadArea) {
      downloadArea.style.display = 'none';
    }
    if (installCmdDiv) {
      installCmdDiv.style.display = 'block';
      const commands = [
        `docker run -d \\`,
          `  --name lemonade-server \\`,
          `  -p 8000:8000 \\`,
          `  -v lemonade-cache:/root/.cache/huggingface \\`,
          `  -v lemonade-llama:/opt/lemonade/llama \\`,
          `  ghcr.io/lemonade-sdk/lemonade-server:latest`
      ];

      installCmdDiv.innerHTML = `<pre><code class="language-bash" id="lmn-install-pre-block"></code></pre>`;

      setTimeout(() => {
        const pre = document.getElementById('lmn-install-pre-block');
        if (pre) {
          pre.innerHTML = commands.map((line, idx) => {
            const safeLine = line.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
            // Only show copy button on first line
            const button = idx === 0 ? `<button class="lmn-copy-btn" title="Copy" onclick="lmnCopyAllInstall(event)">📋</button>` : '';
            return `<div class="lmn-command-line"><span>${safeLine}</span>${button}</div>`;
          }).join('');
        }
      }, 0);
    }

    let dockerNote = `<div class="lmn-note lmn-source-note">To build from source, see the <a href="https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/DOCKER_GUIDE.md" target="_blank">Docker Guide</a>`;
    if (cmdDiv) {
      cmdDiv.innerHTML = dockerNote;
    }

    return;
  } else {
    // Hide Docker section for other platforms
    const dockerSection = document.getElementById('lmn-docker-section');
    if (dockerSection) dockerSection.style.display = 'none';
  }

  // Build from source note (only depends on os and type)
  let notes = '';

  // Add Ubuntu-specific note if Ubuntu is selected
  if (os === 'linux' && distro === 'ubuntu') {
    notes += `<div class="lmn-note">Lemonade is tested on Ubuntu 24.04 LTS but should also work on other versions.</div>`;
  }

  notes += `<div class="lmn-note lmn-source-note">To build from source, see the <a href="https://github.com/lemonade-sdk/lemonade/blob/main/docs/dev-getting-started.md" target="_blank">Developer Guide</a>`;
  if (type === 'app') {
    notes += ` and <a href="https://github.com/lemonade-sdk/lemonade/blob/main/src/app/README.md" target="_blank">App README</a>`;
  }
  notes += `.</div>`;

  if (cmdDiv) {
    cmdDiv.innerHTML = notes;
  }
}

window.lmnCopySnapLine = function(e, idx) {
  e.stopPropagation();
  const pre = document.getElementById('lmn-install-snap-block');
  if (!pre) return;
  const lines = Array.from(pre.querySelectorAll('.lmn-command-line span')).map(span => span.textContent);
  if (lines[idx] !== undefined) {
    navigator.clipboard.writeText(lines[idx]);
    const btn = e.currentTarget;
    const old = btn.textContent;
    btn.textContent = '✔';
    setTimeout(() => { btn.textContent = old; }, 900);
  }
};

window.lmnCopyInstallLine = function(e, idx) {
  e.stopPropagation();
  const pre = document.getElementById('lmn-install-pre-block');
  if (!pre) return;
  const lines = Array.from(pre.querySelectorAll('.lmn-command-line span')).map(span => span.textContent);
  if (lines[idx] !== undefined) {
    navigator.clipboard.writeText(lines[idx]);
    const btn = e.currentTarget;
    const old = btn.textContent;
    btn.textContent = '✔';
    setTimeout(() => { btn.textContent = old; }, 900);
  }
};

window.lmnCopyAllInstall = function(e) {
  e.stopPropagation();
  const pre = document.getElementById('lmn-install-pre-block');
  if (!pre) return;
  const lines = Array.from(pre.querySelectorAll('.lmn-command-line span')).map(span => span.textContent);
  if (lines !== undefined) {
    // Join with newlines instead of commas for proper formatting
    navigator.clipboard.writeText(lines.join('\n'));
    const btn = e.currentTarget;
    const old = btn.textContent;
    btn.textContent = '✔';
    setTimeout(() => { btn.textContent = old; }, 900);
  }
};

function renderQuickStart() {
  const { os, fw, dev } = lmnState;
  const exploreDiv = document.getElementById('lmn-explore-command');
  const exploreSection = document.getElementById('lmn-explore-section');

  if (!exploreDiv || !exploreSection) return;

  // Hide quick start for macOS
  if (os === 'macos') {
    exploreSection.style.display = 'none';
    return;
  }

  let commands = ['lemonade-server -h'];

  if (fw === 'oga') {
    if (dev === 'npu') {
      commands.push('lemonade-server run Llama-3.2-1B-Instruct-NPU');
      commands.push('lemonade-server run Llama-3.2-1B-Instruct-Hybrid');
    } else {
      commands.push('lemonade-server run Llama-3.2-1B-Instruct-CPU');
    }
  } else if (fw === 'llama') {
    if (dev === 'cpu') {
      commands.push('lemonade-server run Gemma-3-4b-it-GGUF --llamacpp cpu');
    } else {
      commands.push('lemonade-server run Gemma-3-4b-it-GGUF');
    }
  } else if (fw === 'flm') {
    commands.push('lemonade-server run Gemma-3-4b-it-FLM');
  }

  exploreSection.style.display = 'block';
  exploreDiv.innerHTML = `<pre><code class="language-bash" id="lmn-explore-pre-block"></code></pre>`;

  // Add contextual notes based on inference engine and device
  let notes = '';

  // NPU driver note
  if (dev === 'npu') {
    notes += `<div class="lmn-note"><strong>Note:</strong> NPU requires an AMD Ryzen AI 300-series PC with Windows 11 and driver installation. Download and install the <a href="${NPU_DRIVER_URL}" target="_blank">NPU Driver</a> before proceeding.</div>`;
  }

  // FastFlowLM Early Access note
  if (fw === 'flm') {
    notes += `<div class="lmn-note"><strong><a href="https://github.com/FastFlowLM/FastFlowLM" target="_blank">FastFlowLM (FLM)</a> support in Lemonade is in Early Access.</strong> FLM is free for non-commercial use, however note that commercial licensing terms apply. Installing an FLM model will automatically launch the FLM installer, which will require you to accept the FLM license terms to continue. Contact <a href="mailto:lemonade@amd.com">lemonade@amd.com</a> for inquiries.</div>`;
  }

  // llama.cpp backend tip for GPU
  if (fw === 'llama' && dev === 'gpu') {
    notes += `<div class="lmn-note"><strong>Tip:</strong> To select a backend, use <code>--llamacpp rocm</code> or <code>--llamacpp vulkan</code></div>`;
    if (os === 'linux') {
      notes += `<div class="lmn-note"><strong>Note:</strong> You may need to run <code>sudo update-pciids</code> for GPU detection on Linux.</div>`;
    }
  }

  // backend config for docker
  if (os === 'docker') {
    commands = [];

    if (fw === 'llama' && dev === 'gpu') {
      commands.push(`docker run -d \\
  --name lemonade-server \\
  -p 8000:8000 \\
  -v lemonade-cache:/root/.cache/huggingface \\
  -v lemonade-llama:/opt/lemonade/llama \\
  -e LEMONADE_LLAMACPP_BACKEND=vulkan \\
  ghcr.io/lemonade-sdk/lemonade-server:latest`);
      notes = `<div class="lmn-note"><strong>Tip:</strong> To select a specific backend, update the LEMONADE_LLAMACPP_BACKEND environment variable: <code>LEMONADE_LLAMACPP_BACKEND=vulkan</code></div>`;
    }

    if (fw === 'llama' && dev === 'cpu') {
      commands.push(`docker run -d \\
  --name lemonade-server \\
  -p 8000:8000 \\
  -v lemonade-cache:/root/.cache/huggingface \\
  -v lemonade-llama:/opt/lemonade/llama \\
  -e LEMONADE_LLAMACPP_BACKEND=cpu \\
  ghcr.io/lemonade-sdk/lemonade-server:latest`);
      notes = `<div class="lmn-note"><strong>Tip:</strong> To select a specific backend, update the LEMONADE_LLAMACPP_BACKEND environment variable: <code>LEMONADE_LLAMACPP_BACKEND=cpu</code></div>`;
    }
  }

  if (notes) {
    exploreDiv.innerHTML += notes;
  }

  setTimeout(() => {
    const pre = document.getElementById('lmn-explore-pre-block');
    if (pre) {
      pre.innerHTML = commands.map((line, idx) => {
        const safeLine = line.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        return `<div class="lmn-command-line"><span>${safeLine}</span><button class="lmn-copy-btn" title="Copy" onclick="lmnCopyLine(event, ${idx})">📋</button></div>`;
      }).join('');
    }
  }, 0);
}

window.lmnCopyLine = function(e, idx) {
  e.stopPropagation();
  const pre = document.getElementById('lmn-explore-pre-block');
  if (!pre) return;
  const lines = Array.from(pre.querySelectorAll('.lmn-command-line span')).map(span => span.textContent);
  if (lines[idx] !== undefined) {
    navigator.clipboard.writeText(lines[idx]);
    const btn = e.currentTarget;
    const old = btn.textContent;
    btn.textContent = '✔';
    setTimeout(() => { btn.textContent = old; }, 900);
  }
};

// Parse URL hash and set appropriate state
function parseHashAndSetState() {
  const hash = window.location.hash.substring(1).toLowerCase(); // Remove # and make lowercase

  if (!hash) return; // No hash, use defaults

  // Handle anchors
  switch (hash) {
    case 'linux':
      lmnSet('os', 'linux');
      break;
    case 'ubuntu':
      lmnSet('os', 'linux');
      lmnSet('distro', 'ubuntu');
      break;
    case 'arch':
      lmnSet('os', 'linux');
      lmnSet('distro', 'arch');
      break;
    case 'debian':
      lmnSet('os', 'linux');
      lmnSet('distro', 'debian');
      break;
    case 'fedora':
      lmnSet('os', 'linux');
      lmnSet('distro', 'fedora');
      break;
    case 'docker':
      lmnSet('os', 'docker');
      break;
    case 'windows':
    case 'win':
      lmnSet('os', 'win');
      break;
    case 'macos':
    case 'mac':
      lmnSet('os', 'macos');
      break;
  }
}

window.lmnInit = function() {
  const installer = document.getElementById('lmn-installer');
  if (installer && !document.getElementById('os-win')) {
    installer.innerHTML = `
      <div class="lmn-content-section">
        <div class="lmn-section-header">Download & Install</div>
        <table class="lmn-installer-table lmn-embedded-table">
          <tr>
            <td class="lmn-label">Platform</td>
            <td id="os-win" class="lmn-active">Windows 11</td>
            <td id="os-linux">Linux</td>
            <td id="os-macos">macOS</td>
            <td id="os-docker">Docker</td>
          </tr>
          <tr id="lmn-install-distro" style="display: none;">
            <td class="lmn-label">Linux Distribution</td>
            <td id="distro-ubuntu" class="lmn-active">Ubuntu 24.04+</td>
            <td id="distro-arch">Arch Linux</td>
            <td id="distro-fedora">Fedora</td>
            <td id="distro-debian">Debian Trixie+</td>
          </tr>
          <tr id="lmn-install-type">
            <td class="lmn-label">Installation Type</td>
            <td id="type-app" colspan="2" class="lmn-active">App + Server</td>
            <td id="type-server" colspan="2">Server Only</td>
          </tr>
        </table>
        <div id="lmn-download-area" class="lmn-download-section">
          <a id="lmn-link" href="#">Download</a>
        </div>
        <div id="lmn-install-commands" class="lmn-command" style="display: none;"></div>
        <div id="lmn-command" class="lmn-command"></div>
      </div>
      <div id="lmn-explore-section" class="lmn-content-section">
        <div class="lmn-section-header lmn-explore-header">Quick Start</div>
        <table class="lmn-installer-table lmn-embedded-table">
          <tr>
            <td class="lmn-label">Inference Engine</td>
            <td id="fw-oga" class="lmn-active">OGA</td>
            <td id="fw-llama">llama.cpp</td>
            <td id="fw-flm">FastFlowLM</td>
          </tr>
          <tr>
            <td class="lmn-label">Device Support</td>
            <td id="dev-npu" class="lmn-active">NPU, Hybrid</td>
            <td id="dev-gpu">GPU</td>
            <td id="dev-cpu">CPU</td>
          </tr>
        </table>
        <div id="lmn-explore-command" class="lmn-command"></div>
      </div>
    `;
  }

  // Listen for hash changes
  window.addEventListener('hashchange', parseHashAndSetState);

  // Parse hash on initial load (after HTML is set up)
  parseHashAndSetState();

  fetchLatestVersion();
  lmnRender();
};
