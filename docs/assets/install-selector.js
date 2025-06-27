// Lemonade Install Selector - Shared JavaScript
// Updated allowlist based on clarified compatibility
const lmnAllowlist = [
  // GUI installer (Windows only)
  // OGA: Hybrid, CPU
  {os:'win', method:'gui', fw:'oga', dev:'hybrid'},
  {os:'win', method:'gui', fw:'oga', dev:'cpu'},
  // llama.cpp: CPU, GPU
  {os:'win', method:'gui', fw:'llama', dev:'cpu'},
  {os:'win', method:'gui', fw:'llama', dev:'gpu'},
  // PyPI and From Source (Windows & Linux)
  // OGA: Hybrid, CPU
  {os:'win', method:'pypi', fw:'oga', dev:'hybrid'},
  {os:'win', method:'pypi', fw:'oga', dev:'cpu'},
  {os:'linux', method:'pypi', fw:'oga', dev:'cpu'},
  {os:'win', method:'src', fw:'oga', dev:'hybrid'},
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

window.lmnState = { os: 'win', type: 'server', method: 'pypi', fw: 'oga', dev: 'cpu' };

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
  ['os-win','os-linux','type-server','type-full','method-gui','method-pypi','method-src','fw-oga','fw-torch','fw-llama','dev-hybrid','dev-cpu','dev-gpu'].forEach(function(id){
    var el = document.getElementById(id);
    if (el) {
      el.className = '';
      el.onclick = null;
    }
  });
  // Set click handlers for all options
  document.getElementById('os-win').onclick = function() { lmnSet('os','win'); };
  document.getElementById('os-linux').onclick = function() { lmnSet('os','linux'); };
  document.getElementById('type-server').onclick = function() { lmnSet('type','server'); };
  document.getElementById('type-full').onclick = function() { lmnSet('type','full'); };
  document.getElementById('method-gui').onclick = function() { lmnSet('method','gui'); };
  document.getElementById('method-pypi').onclick = function() { lmnSet('method','pypi'); };
  document.getElementById('method-src').onclick = function() { lmnSet('method','src'); };
  document.getElementById('fw-oga').onclick = function() { lmnSet('fw','oga'); };
  document.getElementById('fw-torch').onclick = function() { lmnSet('fw','torch'); };
  document.getElementById('fw-llama').onclick = function() { lmnSet('fw','llama'); };
  document.getElementById('dev-hybrid').onclick = function() { lmnSet('dev','hybrid'); };
  document.getElementById('dev-cpu').onclick = function() { lmnSet('dev','cpu'); };
  document.getElementById('dev-gpu').onclick = function() { lmnSet('dev','gpu'); };

  // Highlight active
  document.getElementById('os-'+lmnState.os).classList.add('lmn-active');
  document.getElementById('type-'+lmnState.type).classList.add('lmn-active');
  document.getElementById('method-'+lmnState.method).classList.add('lmn-active');
  document.getElementById('fw-'+lmnState.fw).classList.add('lmn-active');
  document.getElementById('dev-'+lmnState.dev).classList.add('lmn-active');

  // Gray out incompatible options (but keep them clickable)
  const opts = {
    os: ['win','linux'],
    type: ['server','full'],
    method: ['gui','pypi','src'],
    fw: ['oga','torch','llama'],
    dev: ['hybrid','cpu','gpu']
  };
  opts.os.forEach(os => {
    if (!lmnIsAllowed(os, lmnState.method, lmnState.fw, lmnState.dev, lmnState.type)) {
      document.getElementById('os-'+os).classList.add('lmn-disabled');
    }
  });
  opts.type.forEach(type => {
    // Always enabled
  });
  opts.method.forEach(method => {
    if (!lmnIsAllowed(lmnState.os, method, lmnState.fw, lmnState.dev, lmnState.type) && !lmnAlwaysEnabledMethod.includes(method)) {
      document.getElementById('method-'+method).classList.add('lmn-disabled');
    }
  });
  opts.fw.forEach(fw => {
    if (!lmnIsAllowed(lmnState.os, lmnState.method, fw, lmnState.dev, lmnState.type)) {
      document.getElementById('fw-'+fw).classList.add('lmn-disabled');
    }
    // PyTorch is always disabled for server-only
    if (lmnState.type === 'server' && fw === 'torch') {
      document.getElementById('fw-torch').classList.add('lmn-disabled');
    }
  });
  opts.dev.forEach(dev => {
    if (!lmnIsAllowed(lmnState.os, lmnState.method, lmnState.fw, dev, lmnState.type)) {
      document.getElementById('dev-'+dev).classList.add('lmn-disabled');
    }
  });

  // Command rendering
  var cmd = '';
  var link = '';
  var condaBlock = '';
  if (lmnState.method === 'pypi' || lmnState.method === 'src') {
    condaBlock = '<div style="margin-bottom:0.5em"><a href="https://github.com/conda-forge/miniforge?tab=readme-ov-file#install" target="_blank" class="lmn-btn" style="font-size:1.02rem; padding:0.38em 1em;">Download and Install Miniforge</a></div>';
  }
  // Set command and link
  if (lmnState.method === 'gui') {
    if (lmnState.fw === 'oga') {
      if (lmnState.dev === 'hybrid' || lmnState.dev === 'cpu') {
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
      } else if (lmnState.dev === 'hybrid') {
        if (lmnState.method === 'pypi') {
          cmd = lmnState.type === 'server' ? 'pip install lemonade-sdk[oga-hybrid]\nlemonade-install --ryzenai hybrid' : 'pip install lemonade-sdk[dev,oga-hybrid]\nlemonade-install --ryzenai hybrid';
        } else {
          cmd = lmnState.type === 'server' ? 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[oga-hybrid]\nlemonade-install --ryzenai hybrid' : 'git clone https://github.com/lemonade-sdk/lemonade.git\ncd lemonade\npip install -e .[dev,oga-hybrid]\nlemonade-install --ryzenai hybrid';
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
    label = cmdLines > 1 ? 'Run these Commands:' : 'Run this Command:';
  } else {
    label = '';
  }
  var cmdDiv = document.getElementById('lmn-command');
  var labelDiv = document.getElementById('lmn-command-label');
  labelDiv.textContent = label;
  if (link && cmd !== '' && lmnState.method === 'gui') {
    cmdDiv.innerHTML = '<div class="lmn-command"><a id="lmn-link" href="'+link+'" class="lmn-btn">'+cmd+'</a></div>';
  } else if (cmd) {
    var fullBlock = (condaBlock ? condaBlock : '') + '<pre><code class="language-bash" id="lmn-pre-block"></code></pre>';
    cmdDiv.innerHTML = '<div class="lmn-command">'+fullBlock+'</div>';
    // Add hybrid note if Hybrid is selected with PyPI or From Source
    if ((lmnState.method === 'pypi' || lmnState.method === 'src') && lmnState.dev === 'hybrid') {
      cmdDiv.innerHTML += '<div style="margin-top:0.7em; color:#222; font-size:1.04rem; font-style:italic;">Hybrid requires an AMD Ryzen AI 300-series PC with Windows 11.</div>';
    }
    // Add llama.cpp benchmarking note if CPU is selected with PyPI or From Source and Full SDK
    if ((lmnState.method === 'pypi' || lmnState.method === 'src') && lmnState.fw === 'llama' && lmnState.dev === 'cpu' && lmnState.type === 'full') {
      cmdDiv.innerHTML += '<div style="margin-top:0.7em; color:#222; font-size:1.04rem; font-style:italic;">For benchmarking, see the llamacpp instructions in <a href="https://github.com/lemonade-sdk/lemonade/blob/main/docs/llamacpp.md" target="_blank" style="color:#e6b800; text-decoration:underline;">llamacpp.md</a></div>';
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
  } else {
    cmdDiv.innerHTML = '';
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
          <td id="dev-hybrid" class="lmn-active" onclick="lmnSet('dev','hybrid')">Hybrid</td>
          <td id="dev-cpu" onclick="lmnSet('dev','cpu')">CPU</td>
          <td id="dev-gpu" onclick="lmnSet('dev','gpu')">GPU</td>
        </tr>
      </table>
      <div class="lmn-command-area" style="margin-top:1em;">
        <b id="lmn-command-label">Run this Command:</b>
        <div id="lmn-command" class="lmn-command">
          <a id="lmn-link" href="https://github.com/lemonade-sdk/lemonade/releases/latest/download/lemonade_server_installer.exe">Download Lemonade Server Installer (.exe)</a>
        </div>
      </div>
    `;
  }
  lmnRender();
};
