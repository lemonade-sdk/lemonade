// Lemonade Install Selector - Shared JavaScript
// Updated allowlist based on clarified compatibility
const lmnAllowlist = [
  // GUI installer (Windows only)
  // OGA: NPU, CPU
  {os:'win', method:'gui', fw:'oga', dev:'npu'},
  {os:'win', method:'gui', fw:'oga', dev:'cpu'},
  // llama.cpp: CPU, GPU
  {os:'win', method:'gui', fw:'llama', dev:'cpu'},
  {os:'win', method:'gui', fw:'llama', dev:'gpu'},
  // PyPI and From Source (Windows & Linux)
  // OGA: NPU, CPU
  {os:'win', method:'pypi', fw:'oga', dev:'npu'},
  {os:'win', method:'pypi', fw:'oga', dev:'cpu'},
  {os:'linux', method:'pypi', fw:'oga', dev:'cpu'},
  {os:'win', method:'src', fw:'oga', dev:'npu'},
  {os:'win', method:'src', fw:'oga', dev:'cpu'},
  {os:'linux', method:'src', fw:'oga', dev:'cpu'},
  // PyTorch: CPU only (GPU removed)
  {os:'win', method:'pypi', fw:'torch', dev:'cpu'},
  {os:'linux', method:'pypi', fw:'torch', dev:'cpu'},
  {os:'win', method:'src', fw:'torch', dev:'cpu'},
  {os:'linux', method:'src', fw:'torch', dev:'cpu'},
  // llama.cpp: CPU, GPU
  {os:'win', method:'pypi', fw:'llama', dev:'cpu'},
  {os:'win', method:'pypi', fw:'llama', dev:'gpu'},
  {os:'linux', method:'pypi', fw:'llama', dev:'cpu'},
  {os:'linux', method:'pypi', fw:'llama', dev:'gpu'},
  {os:'win', method:'src', fw:'llama', dev:'cpu'},
  {os:'win', method:'src', fw:'llama', dev:'gpu'},
  {os:'linux', method:'src', fw:'llama', dev:'cpu'},
  {os:'linux', method:'src', fw:'llama', dev:'gpu'},
];

const lmnAlwaysEnabledMethod = ['pypi', 'src'];

window.lmnState = { os: 'win', type: 'server', method: 'gui', fw: 'oga', dev: 'npu' };

function lmnIsAllowed(os, method, fw, dev, type) {
  // PyTorch not allowed for server-only
  if (type === 'server' && fw === 'torch') return false;
  // GUI not allowed for full SDK
  if (type === 'full' && method === 'gui') return false;
  // GUI only for Windows
  if (method === 'gui' && os !== 'win') return false;
  // Otherwise use allowlist
  return lmnAllowlist.some(c => c.os === os && c.method === method && c.fw === fw && c.dev === dev);
}

function lmnFindClosestValid(newState) {
  // Try to find a valid combination with the changed field, prefer keeping other fields the same
  let candidates = lmnAllowlist.filter(c => {
    for (let k in newState) {
      if (newState[k] !== undefined && c[k] !== newState[k]) return false;
    }
    return true;
  });
  if (candidates.length > 0) return candidates[0];
  // If not found, relax one field at a time (priority: method, fw, dev, os)
  const keys = ['method','fw','dev','os'];
  for (let i=0; i<keys.length; ++i) {
    let relaxed = {...newState};
    delete relaxed[keys[i]];
    candidates = lmnAllowlist.filter(c => {
      for (let k in relaxed) {
        if (relaxed[k] !== undefined && c[k] !== relaxed[k]) return false;
      }
      return true;
    });
    if (candidates.length > 0) return candidates[0];
  }
  // Fallback: first allowlist entry for the current type/os
  if (newState.type) {
    let fallback = lmnAllowlist.find(c => c.os === newState.os);
    if (fallback) return fallback;
  }
  return lmnAllowlist[0];
}

window.lmnSet = function(type, val) {
  let newState = {...lmnState, [type]: val};
  
  // Handle special cases first
  if (type === 'fw' && val === 'torch') {
    // PyTorch only works with Full SDK, so force that
    newState.type = 'full';
  } else if (type === 'method' && val === 'gui') {
    // GUI only works with Server Only, so force that
    newState.type = 'server';
  } else if (type === 'type' && val === 'server' && newState.fw === 'torch') {
    // If switching to Server Only but PyTorch is selected, switch to OGA
    newState.fw = 'oga';
  } else if (type === 'type' && val === 'full' && newState.method === 'gui') {
    // If switching to Full SDK but GUI is selected, switch to PyPI
    newState.method = 'pypi';
  }
  
  // Always ensure the clicked option is selected in the resulting state
  if (!lmnIsAllowed(newState.os, newState.method, newState.fw, newState.dev, newState.type)) {
    // Find the first valid combination that includes the user's chosen value for the changed type
    let candidates = lmnAllowlist.filter(c => {
      if (type === 'os' && c.os !== val) return false;
      if (type === 'method' && c.method !== val) return false;
      if (type === 'fw' && c.fw !== val) return false;
      if (type === 'dev' && c.dev !== val) return false;
      // Respect type restrictions (use the potentially updated type)
      if (!lmnIsAllowed(c.os, c.method, c.fw, c.dev, newState.type)) return false;
      return true;
    });
    
    if (candidates.length > 0) {
      let fallback = candidates[0];
      // Compose new state with the user's intent, keeping the clicked value
      newState = {
        os: (type === 'os' ? val : fallback.os),
        type: newState.type, // Keep the potentially updated type
        method: (type === 'method' ? val : fallback.method),
        fw: (type === 'fw' ? val : fallback.fw),
        dev: (type === 'dev' ? val : fallback.dev)
      };
    } else {
      // Fallback: find any valid combination for the current OS and updated type
      let osFallback = lmnAllowlist.find(c => c.os === newState.os && lmnIsAllowed(c.os, c.method, c.fw, c.dev, newState.type));
      if (osFallback) {
        newState = {
          os: newState.os,
          type: newState.type,
          method: (type === 'method' ? val : osFallback.method),
          fw: (type === 'fw' ? val : osFallback.fw),
          dev: (type === 'dev' ? val : osFallback.dev)
        };
      } else {
        // Last resort: use first allowlist entry but keep user's choice
        let lastResort = lmnAllowlist[0];
        newState = {
          os: (type === 'os' ? val : lastResort.os),
          type: newState.type,
          method: (type === 'method' ? val : lastResort.method),
          fw: (type === 'fw' ? val : lastResort.fw),
          dev: (type === 'dev' ? val : lastResort.dev)
        };
      }
    }
  }
  
  window.lmnState = {...newState};
  lmnRender();
};

window.lmnRender = function() {
  // Reset all
  ['os-win','os-linux','type-server','type-full','method-gui','method-pypi','method-src','fw-oga','fw-torch','fw-llama','dev-npu','dev-cpu','dev-gpu'].forEach(function(id){
    var el = document.getElementById(id);
    if (el) {
      el.className = '';
      el.onclick = null;
    }
  });
  // Set click handlers for all options
  var osWin = document.getElementById('os-win');
  if (osWin) osWin.onclick = function() { lmnSet('os','win'); };
  var osLinux = document.getElementById('os-linux');
  if (osLinux) osLinux.onclick = function() { lmnSet('os','linux'); };
  var typeServer = document.getElementById('type-server');
  if (typeServer) typeServer.onclick = function() { lmnSet('type','server'); };
  var typeFull = document.getElementById('type-full');
  if (typeFull) typeFull.onclick = function() { lmnSet('type','full'); };
  var methodGui = document.getElementById('method-gui');
  if (methodGui) methodGui.onclick = function() { lmnSet('method','gui'); };
  var methodPypi = document.getElementById('method-pypi');
  if (methodPypi) methodPypi.onclick = function() { lmnSet('method','pypi'); };
  var methodSrc = document.getElementById('method-src');
  if (methodSrc) methodSrc.onclick = function() { lmnSet('method','src'); };
  var fwOga = document.getElementById('fw-oga');
  if (fwOga) fwOga.onclick = function() { lmnSet('fw','oga'); };
  var fwTorch = document.getElementById('fw-torch');
  if (fwTorch) fwTorch.onclick = function() { lmnSet('fw','torch'); };
  var fwLlama = document.getElementById('fw-llama');
  if (fwLlama) fwLlama.onclick = function() { lmnSet('fw','llama'); };
  var devNpu = document.getElementById('dev-npu');
  if (devNpu) devNpu.onclick = function() { lmnSet('dev','npu'); };
  var devCpu = document.getElementById('dev-cpu');
  if (devCpu) devCpu.onclick = function() { lmnSet('dev','cpu'); };
  var devGpu = document.getElementById('dev-gpu');
  if (devGpu) devGpu.onclick = function() { lmnSet('dev','gpu'); };

  // Highlight active
  var osElement = document.getElementById('os-'+lmnState.os);
  if (osElement) osElement.classList.add('lmn-active');
  var typeElement = document.getElementById('type-'+lmnState.type);
  if (typeElement) typeElement.classList.add('lmn-active');
  var methodElement = document.getElementById('method-'+lmnState.method);
  if (methodElement) methodElement.classList.add('lmn-active');
  var fwElement = document.getElementById('fw-'+lmnState.fw);
  if (fwElement) fwElement.classList.add('lmn-active');
  var devElement = document.getElementById('dev-'+lmnState.dev);
  if (devElement) devElement.classList.add('lmn-active');

  // Gray out incompatible options (but keep them clickable)
  const opts = {
    os: ['win','linux'],
    type: ['server','full'],
    method: ['gui','pypi','src'],
    fw: ['oga','torch','llama'],
    dev: ['npu','cpu','gpu']
  };
  opts.os.forEach(os => {
    if (!lmnIsAllowed(os, lmnState.method, lmnState.fw, lmnState.dev, lmnState.type)) {
      var osEl = document.getElementById('os-'+os);
      if (osEl) osEl.classList.add('lmn-disabled');
    }
  });
  opts.type.forEach(type => {
    // Always enabled
  });
  opts.method.forEach(method => {
    if (!lmnIsAllowed(lmnState.os, method, lmnState.fw, lmnState.dev, lmnState.type) && !lmnAlwaysEnabledMethod.includes(method)) {
      var methodEl = document.getElementById('method-'+method);
      if (methodEl) methodEl.classList.add('lmn-disabled');
    }
  });
  opts.fw.forEach(fw => {
    if (!lmnIsAllowed(lmnState.os, lmnState.method, fw, lmnState.dev, lmnState.type)) {
      var fwEl = document.getElementById('fw-'+fw);
      if (fwEl) fwEl.classList.add('lmn-disabled');
    }
    // PyTorch is always disabled for server-only
    if (lmnState.type === 'server' && fw === 'torch') {
      var fwTorchEl = document.getElementById('fw-torch');
      if (fwTorchEl) fwTorchEl.classList.add('lmn-disabled');
    }
  });
  opts.dev.forEach(dev => {
    if (!lmnIsAllowed(lmnState.os, lmnState.method, lmnState.fw, dev, lmnState.type)) {
      var devEl = document.getElementById('dev-'+dev);
      if (devEl) devEl.classList.add('lmn-disabled');
    }
  });

  // Command rendering
  
  // Generate badges
  function generateBadges() {
    var badges = '';
    
    // Python version badge
    var pythonVersions = '';
    if (lmnState.dev === 'npu' || lmnState.method === 'gui') {
      pythonVersions = '3.10';
    } else {
      pythonVersions = '3.10--3.13';
    }
    badges += '<img src="https://img.shields.io/badge/Python-' + pythonVersions + '-blue" alt="Python versions" style="margin-right: 0.5em;">';
    
    // OS version badge
    var osVersions = '';
    if (lmnState.os === 'linux') {
      osVersions = 'Ubuntu%2024.04%20%7C%2025.04';
    } else { // windows
      osVersions = 'Windows%2011';
    }
    badges += '<img src="https://img.shields.io/badge/OS-' + osVersions + '-green" alt="OS versions">';
    
    return badges;
  }
  
  // Generate explore commands
  function generateExploreCommands() {
    var commands = [];
    var prefix = (lmnState.type === 'server')
      ? (lmnState.method === 'gui' ? 'lemonade-server' : 'lemonade-server-dev')
      : 'lemonade-server-dev';

    if (lmnState.type === 'full') {
      commands.push('lemonade -h');
      commands.push('lemonade-server-dev -h');
    } else {
      commands.push(prefix + ' -h');
    }

    if (lmnState.fw === 'oga') {
      if (lmnState.dev === 'npu') {
        commands.push(prefix + ' run Llama-3.2-1B-Instruct-NPU');
        commands.push(prefix + ' run Llama-3.2-1B-Instruct-Hybrid');
      } else {
        commands.push(prefix + ' run Llama-3.2-1B-Instruct-CPU');
      }
    } else if (lmnState.fw === 'llama') {
      commands.push(prefix + ' run Gemma-3-4b-it-GGUF');
    } else if (lmnState.fw === 'torch') {
      // No command for PyTorch, refer user to dev_cli documentation; leave blank
    } else {
      commands.push(prefix + ' run Gemma-3-4b-it-GGUF');
    }

    return commands;
  }
  
  var cmd = '';
  var link = '';
  var condaBlock = '';
  if (lmnState.method === 'pypi' || lmnState.method === 'src') {
    condaBlock = '<div style="margin-bottom:0.5em"><a href="https://github.com/conda-forge/miniforge?tab=readme-ov-file#install" target="_blank" class="lmn-btn" style="font-size:1.02rem; padding:0.38em 1em;">Download and Install Miniforge</a></div>';
  }
  // Set command and link
  if (lmnState.method === 'gui') {
    if (lmnState.fw === 'oga') {
      if (lmnState.dev === 'npu' || lmnState.dev === 'cpu') {
        cmd = 'Download Lemonade Server Installer (.exe)';
        link = 'https://github.com/lemonade-sdk/lemonade/releases/latest/download/lemonade_server_installer.exe';
      }
    } else if (lmnState.fw === 'llama') {
      if (lmnState.dev === 'cpu' || lmnState.dev === 'gpu') {
        cmd = 'Download Lemonade Server Installer (.exe)';
        link = 'https://github.com/lemonade-sdk/lemonade/releases/latest/download/lemonade_server_installer.exe';
      }
    }
  } else if (lmnState.method === 'pypi' || lmnState.method === 'src') {
    if (lmnState.fw === 'oga') {
      if (lmnState.dev === 'cpu') {
        if (lmnState.method === 'pypi') {
          cmd = lmnState.type === 'server' ? 'pip install lemonade-sdk[oga-cpu]' : 'pip install lemonade-sdk[dev,oga-cpu]';
        } else {
          cmd = lmnState.type === 'server' ? 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[oga-cpu]' : 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[dev,oga-cpu]';
        }
      } else if (lmnState.dev === 'npu') {
        if (lmnState.method === 'pypi') {
          cmd = lmnState.type === 'server' ? 'pip install lemonade-sdk[oga-ryzenai] --extra-index-url=https://pypi.amd.com/simple' : 'pip install lemonade-sdk[dev,oga-ryzenai] --extra-index-url=https://pypi.amd.com/simple';
        } else {
          cmd = lmnState.type === 'server' ? 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[oga-ryzenai] --extra-index-url=https://pypi.amd.com/simple' : 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[dev,oga-ryzenai] --extra-index-url=https://pypi.amd.com/simple';
        }
      }
    } else if (lmnState.fw === 'torch') {
      // PyTorch not available for server-only
      if (lmnState.type === 'full' && (lmnState.dev === 'cpu' || lmnState.dev === 'gpu')) {
        if (lmnState.method === 'pypi') {
          cmd = 'pip install lemonade-sdk[dev]';
        } else {
          cmd = 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[dev]';
        }
      }
    } else if (lmnState.fw === 'llama') {
      if (lmnState.dev === 'cpu' || lmnState.dev === 'gpu') {
        if (lmnState.method === 'pypi') {
          cmd = lmnState.type === 'server' ? 'pip install lemonade-sdk' : 'pip install lemonade-sdk[dev]';
        } else {
          cmd = lmnState.type === 'server' ? 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .' : 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[dev]';
        }
        // Add ROCm requirement for llama.cpp GPU Linux installations (GUI)
        if (lmnState.os === 'linux' && lmnState.dev === 'gpu') {
          cmd = 'sudo update-pciids\n' + cmd;
        }
      }
    }
  }
  // Now set the label based on what will be rendered
  var label = '';
  if (link && cmd !== '' && lmnState.method === 'gui') {
    label = 'Install this tool:';
  } else if (cmd) {
    var gitCloneLines = (lmnState.method === 'src') ? 2 : 0; // git clone + cd
    var cmdLines = (condaBlock ? 2 : 0) + gitCloneLines + (cmd ? cmd.split('\n').length : 0);
    label = cmdLines > 1 ? 'Run these Commands to Install:' : 'Run this Command to Install:';
  } else {
    label = '';
  }
  var cmdDiv = document.getElementById('lmn-command');
  var downloadArea = document.getElementById('lmn-download-area');
  var badgesDiv = document.getElementById('lmn-badges');
  var exploreDiv = document.getElementById('lmn-explore-command');
  var exploreSection = document.getElementById('lmn-explore-section');
  
  // Render badges
  if (badgesDiv) {
    badgesDiv.innerHTML = generateBadges();
  }
  
  // Handle GUI downloads vs commands
  if (link && cmd !== '' && lmnState.method === 'gui') {
    // Show download area, hide command area
    if (downloadArea) {
      downloadArea.style.display = 'block';
      var linkElement = document.getElementById('lmn-link');
      if (linkElement) {
        linkElement.href = link;
        linkElement.textContent = cmd;
      }
    }
    if (cmdDiv) {
      cmdDiv.innerHTML = '';
      // Add NPU driver requirement for GUI installations
      if (lmnState.dev === 'npu') {
        cmdDiv.innerHTML += '<div style="margin-top:0.7em; color:#666; font-size:1.04rem;"><strong>Note:</strong> NPU requires an AMD Ryzen AI 300-series PC with Windows 11 and driver installation. Download and install the <a href="' + NPU_DRIVER_URL + '" target="_blank" style="color:#666; text-decoration:underline;">NPU Driver</a> before proceeding.</div>';
      }
    }
  } else if (cmd) {
    // Show command area, hide download area
    if (downloadArea) {
      downloadArea.style.display = 'none';
    }
    if (cmdDiv) {
      var fullBlock = (condaBlock ? condaBlock : '') + '<pre><code class="language-bash" id="lmn-pre-block"></code></pre>';
      cmdDiv.innerHTML = '<div class="lmn-command">'+fullBlock+'</div>';
      
      // Add a note if NPU is selected
      if (lmnState.dev === 'npu') {
        cmdDiv.innerHTML += '<div style="margin-top:0.7em; color:#666; font-size:1.04rem;"><strong>Note:</strong> NPU requires an AMD Ryzen AI 300-series PC with Windows 11 and driver installation. Download and install the <a href="' + NPU_DRIVER_URL + '" target="_blank" style="color:#666; text-decoration:underline;">NPU Driver</a> before proceeding.</div>';
      }
      
      // Render command lines with copy buttons
      setTimeout(function() {
        var pre = document.getElementById('lmn-pre-block');
        if (pre) {
          var lines = [];
          if (condaBlock) {
            lines.push('conda create -n lemon python=3.10');
            lines.push('conda activate lemon');
          }
          cmd.split('\n').forEach(function(line) { lines.push(line); });
          pre.innerHTML = lines.map(function(line, idx) {
            var safeLine = line.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
            return '<div class="lmn-command-line"><span>'+safeLine+'</span><button class="lmn-copy-btn" title="Copy" onclick="lmnCopyLine(event, '+idx+')">📋</button></div>';
          }).join('');
        }
      }, 0);
    }
  } else {
    if (cmdDiv) cmdDiv.innerHTML = '';
    if (downloadArea) downloadArea.style.display = 'none';
  }
  
  // Render explore commands
  if (exploreDiv && exploreSection) {
    var exploreCommands = generateExploreCommands();
    if (exploreCommands.length > 0) {
      exploreSection.style.display = 'block';
      exploreDiv.innerHTML = '<pre><code class="language-bash" id="lmn-explore-pre-block"></code></pre>';
      
      // Add llama.cpp tip below Quick Start commands
      if (lmnState.fw === 'llama' && lmnState.dev === 'gpu') {
        exploreDiv.innerHTML += '<div style="margin-top:0.7em; color:#666; font-size:1.04rem;"><strong>Tip:</strong> Use <span style="font-family:monospace; background:#f5f5f5; padding:2px 4px; border-radius:3px;">--llamacpp rocm</span> or <span style="font-family:monospace; background:#f5f5f5; padding:2px 4px; border-radius:3px;">--llamacpp vulkan</span> to select backends.</div>';
      }

      // Add Pytorch and CPU tip to point to guide
      if (lmnState.fw === 'torch' && lmnState.dev === 'cpu') {
        exploreDiv.innerHTML += '<div style="margin-top:0.7em; color:#666; font-size:1.04rem;"><strong>Tip:</strong> For CPU usage, refer to the <a href="https://github.com/lemonade-sdk/lemonade/tree/main/docs/dev_cli" target="_blank" style="color:#666; text-decoration:underline;">Developer CLI Guide</a>.</div>';
      }

      setTimeout(function() {
        var explorePre = document.getElementById('lmn-explore-pre-block');
        if (explorePre) {
          explorePre.innerHTML = exploreCommands.map(function(line, idx) {
            var safeLine = line.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
            return '<div class="lmn-command-line"><span>'+safeLine+'</span><button class="lmn-copy-btn" title="Copy" onclick="lmnCopyExploreLine(event, '+idx+')">📋</button></div>';
          }).join('');
        }
      }, 0);
    } else {
      exploreSection.style.display = 'none';
    }
  }
};

window.lmnCopyLine = function(e, idx) {
  e.stopPropagation();
  var pre = document.getElementById('lmn-pre-block');
  if (!pre) return;
  var lines = Array.from(pre.querySelectorAll('.lmn-command-line span')).map(function(span) { return span.textContent; });
  if (lines[idx] !== undefined) {
    navigator.clipboard.writeText(lines[idx]);
    var btn = e.currentTarget;
    var old = btn.textContent;
    btn.textContent = '✔';
    setTimeout(function() { btn.textContent = old; }, 900);
  }
};

window.lmnCopyExploreLine = function(e, idx) {
  e.stopPropagation();
  var pre = document.getElementById('lmn-explore-pre-block');
  if (!pre) return;
  var lines = Array.from(pre.querySelectorAll('.lmn-command-line span')).map(function(span) { return span.textContent; });
  if (lines[idx] !== undefined) {
    navigator.clipboard.writeText(lines[idx]);
    var btn = e.currentTarget;
    var old = btn.textContent;
    btn.textContent = '✔';
    setTimeout(function() { btn.textContent = old; }, 900);
  }
};

// NPU Driver download URL - keep in sync with src/lemonade_install/install.py
const NPU_DRIVER_URL = "https://account.amd.com/en/forms/downloads/ryzenai-eula-public-xef.html?filename=NPU_RAI1.5_280_WHQL.zip";

// Initialize when DOM is ready
window.lmnInit = function() {
  // Check if we need to create the table (for standalone page)
  const installer = document.getElementById('lmn-installer');
  if (installer && !document.getElementById('os-win')) {
    // Create the table HTML
    installer.innerHTML = `
      <table class="lmn-installer-table">
        <tr>
          <td class="lmn-label">Operating System</td>
          <td id="os-win" class="lmn-active" onclick="lmnSet('os','win')">Windows</td>
          <td id="os-linux" onclick="lmnSet('os','linux')">Linux</td>
        </tr>
        <tr>
          <td class="lmn-label">Installation Type</td>
          <td id="type-server" class="lmn-active" onclick="lmnSet('type','server')">Server Only</td>
          <td id="type-full" onclick="lmnSet('type','full')">Full SDK</td>
        </tr>
        <tr>
          <td class="lmn-label">Installation Method</td>
          <td id="method-gui" class="lmn-active" onclick="lmnSet('method','gui')">GUI .exe</td>
          <td id="method-pypi" onclick="lmnSet('method','pypi')">PyPI</td>
          <td id="method-src" onclick="lmnSet('method','src')">From Source</td>
        </tr>
        <tr>
          <td class="lmn-label">Inference Engine</td>
          <td id="fw-oga" class="lmn-active" onclick="lmnSet('fw','oga')">OGA</td>
          <td id="fw-llama" onclick="lmnSet('fw','llama')">llama.cpp</td>
          <td id="fw-torch" onclick="lmnSet('fw','torch')">PyTorch</td>
        </tr>
        <tr>
          <td class="lmn-label">Device Support</td>
          <td id="dev-npu" class="lmn-active" onclick="lmnSet('dev','npu')">NPU, Hybrid</td>
          <td id="dev-cpu" onclick="lmnSet('dev','cpu')">CPU</td>
          <td id="dev-gpu" onclick="lmnSet('dev','gpu')">GPU</td>
        </tr>
      </table>
      <div class="lmn-content-section">
        <div class="lmn-section-header">
          Installation Instructions
        </div>
        <div id="lmn-badges" class="lmn-badges"></div>
        <div id="lmn-install-content">
          <div id="lmn-download-area" class="lmn-download-section" style="display: none;">
            <a id="lmn-link" href="https://github.com/lemonade-sdk/lemonade/releases/latest/download/lemonade_server_installer.exe">Download Lemonade Server Installer (.exe)</a>
          </div>
          <div id="lmn-command" class="lmn-command"></div>
        </div>
      </div>
      <div id="lmn-explore-section" class="lmn-content-section" style="margin-top: 1.5em;">
        <div class="lmn-section-header lmn-explore-header">
          Quick Start
        </div>
        <div id="lmn-explore-command" class="lmn-command"></div>
      </div>
    `;
  }
  lmnRender();
};
