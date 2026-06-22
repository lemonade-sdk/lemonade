// Shared navbar for the Lemonade homepage and static HTML pages
// (marketplace.html, models.html, news/, etc.). The Zensical docs site
// renders its own header, restyled in docs/assets/zest-theme.css.

function isHomepage() {
  const path = window.location.pathname.replace(/\/+$/, '');
  return path === '' ||
    path === '/index' ||
    path === '/index.html' ||
    path.endsWith('/docs') ||
    path.endsWith('/docs/index') ||
    path.endsWith('/docs/index.html');
}

function createNavbar(basePath = '') {
  const showPersonaToggle = isHomepage();
  const showDownloadButton = !showPersonaToggle;
  const personaToggleHtml = showPersonaToggle ? `
        <div class="navbar-persona-toggle" role="group" aria-label="Choose audience">
          <button class="navbar-persona-option" type="button" data-persona-option="people">Users</button>
          <button class="navbar-persona-option" type="button" data-persona-option="developers">Devs</button>
        </div>` : '';
  const downloadButtonHtml = showDownloadButton ? `
        <a class="navbar-install-btn" href="${basePath}index.html#getting-started">Download</a>` : '';

  return `
    <nav class="navbar" id="navbar">
      <div class="navbar-brand">
        <span class="brand-title"><a href="https://lemonade-server.ai"><img class="brand-icon" src="${basePath}favicon.ico" alt="" />Lemonade</a></span>
      </div>
      <div class="navbar-links" id="navbarLinks">
        <a href="${basePath}docs/">Docs</a>
        <a href="${basePath}models.html">Models</a>
        <a href="${basePath}marketplace.html">Apps</a>
        <a href="${basePath}news/">News</a>
      </div>
      <div class="navbar-actions">
        <div class="navbar-socials" aria-label="Community links">
          <a class="navbar-social-link navbar-social-link-count" href="https://github.com/lemonade-sdk/lemonade" target="_blank" rel="noopener" aria-label="Lemonade on GitHub">
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 .5a12 12 0 0 0-3.79 23.38c.6.11.82-.26.82-.58v-2.17c-3.34.73-4.04-1.42-4.04-1.42-.55-1.4-1.34-1.77-1.34-1.77-1.1-.75.08-.73.08-.73 1.21.08 1.85 1.24 1.85 1.24 1.08 1.85 2.83 1.32 3.52 1 .11-.78.42-1.32.76-1.62-2.66-.3-5.46-1.33-5.46-5.93 0-1.31.47-2.38 1.24-3.22-.12-.3-.54-1.52.12-3.18 0 0 1.01-.32 3.3 1.23a11.5 11.5 0 0 1 6.02 0c2.29-1.55 3.3-1.23 3.3-1.23.66 1.66.24 2.88.12 3.18.77.84 1.24 1.91 1.24 3.22 0 4.61-2.8 5.63-5.47 5.93.43.37.81 1.1.81 2.22v3.3c0 .32.22.7.83.58A12 12 0 0 0 12 .5Z"></path></svg>
            <span id="githubStarsCount" data-community-stat="github">Stars</span>
          </a>
          <a class="navbar-social-link navbar-social-link-count" href="https://discord.gg/5xXzkMu8Zk" target="_blank" rel="noopener" aria-label="Lemonade Discord">
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M19.54 5.33A16.9 16.9 0 0 0 15.33 4l-.2.38c1.46.36 2.14.88 2.14.88a14.5 14.5 0 0 0-6.77-.26 12.3 12.3 0 0 0-3.77.26s.7-.55 2.25-.9L8.84 4a17 17 0 0 0-4.22 1.33C1.95 9.3 1.22 13.17 1.58 17c1.78 1.3 3.5 2.1 5.2 2.62l.63-.85a6.8 6.8 0 0 1-1.64-.78l.4-.3c3.16 1.48 6.6 1.48 9.72 0l.4.3c-.52.32-1.06.58-1.64.78l.63.85c1.7-.52 3.43-1.31 5.2-2.62.43-4.43-.72-8.26-2.94-11.67ZM8.52 14.64c-.95 0-1.73-.88-1.73-1.96s.76-1.96 1.73-1.96c.96 0 1.75.88 1.73 1.96 0 1.08-.77 1.96-1.73 1.96Zm6.96 0c-.95 0-1.73-.88-1.73-1.96s.76-1.96 1.73-1.96c.96 0 1.74.88 1.73 1.96 0 1.08-.77 1.96-1.73 1.96Z"></path></svg>
            <span data-community-stat="discord">Members</span>
          </a>
          <a class="navbar-social-link" href="https://x.com/AMDDevCentral" target="_blank" rel="noopener" aria-label="Lemonade on X">
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M18.9 2.25h3.38l-7.38 8.43 8.68 11.07h-6.8l-5.32-6.72-6.1 6.72H1.98l7.9-8.7L1.55 2.25h6.97l4.82 6.17 5.56-6.17Zm-1.18 17.55h1.87L7.48 4.1H5.47l12.25 15.7Z"></path></svg>
          </a>
          <a class="navbar-social-link" href="https://www.youtube.com/@AMDDeveloperCentral" target="_blank" rel="noopener" aria-label="Lemonade on YouTube">
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M23.5 6.2a3 3 0 0 0-2.1-2.13C19.56 3.58 12 3.58 12 3.58s-7.56 0-9.4.49A3 3 0 0 0 .5 6.2 31.4 31.4 0 0 0 0 12a31.4 31.4 0 0 0 .5 5.8 3 3 0 0 0 2.1 2.13c1.84.49 9.4.49 9.4.49s7.56 0 9.4-.49a3 3 0 0 0 2.1-2.13A31.4 31.4 0 0 0 24 12a31.4 31.4 0 0 0-.5-5.8ZM9.55 15.45v-6.9L15.82 12l-6.27 3.45Z"></path></svg>
          </a>
        </div>
${personaToggleHtml}
${downloadButtonHtml}
        <button class="navbar-toggle" id="navbarToggle" type="button" aria-label="Toggle navigation menu" aria-expanded="false" aria-controls="navbarLinks">
          <span class="navbar-toggle-bar"></span>
          <span class="navbar-toggle-bar"></span>
          <span class="navbar-toggle-bar"></span>
        </button>
      </div>
    </nav>
  `;
}

function initializeNavbar(basePath = '') {
  const navbarContainer = document.querySelector('.navbar-placeholder');
  if (!navbarContainer) {
    console.warn('Navbar placeholder not found');
    return;
  }
  navbarContainer.innerHTML = createNavbar(basePath);

  const toggle = document.getElementById('navbarToggle');
  const links = document.getElementById('navbarLinks');
  if (!toggle || !links) return;

  function setOpen(open) {
    links.classList.toggle('is-open', open);
    toggle.classList.toggle('is-open', open);
    toggle.setAttribute('aria-expanded', open ? 'true' : 'false');
  }

  toggle.addEventListener('click', function() {
    setOpen(!links.classList.contains('is-open'));
  });
  links.addEventListener('click', function(e) {
    if (e.target.tagName === 'A') setOpen(false);
  });

  const personaButtons = navbarContainer.querySelectorAll('[data-persona-option]');
  if (personaButtons.length) {
    const storedPersona = readPersona();
    setPersona(storedPersona, false);
  }

  personaButtons.forEach(function(button) {
    button.addEventListener('click', function() {
      setPersona(button.getAttribute('data-persona-option'), true);
      setOpen(false);
    });
  });

  function readPersona() {
    try {
      return localStorage.getItem('lemonade-persona') || 'people';
    } catch (error) {
      return 'people';
    }
  }

  function setPersona(persona, persist) {
    var nextPersona = persona === 'developers' ? 'developers' : 'people';
    document.documentElement.setAttribute('data-persona', nextPersona);
    if (nextPersona === 'developers') {
      document.documentElement.setAttribute('data-md-color-scheme', 'zest-dark');
    } else {
      document.documentElement.removeAttribute('data-md-color-scheme');
    }
    personaButtons.forEach(function(button) {
      var active = button.getAttribute('data-persona-option') === nextPersona;
      button.classList.toggle('is-active', active);
      button.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
    if (persist) {
      try { localStorage.setItem('lemonade-persona', nextPersona); } catch (error) {}
    }
    window.dispatchEvent(new CustomEvent('lemonadePersonaChange', { detail: { persona: nextPersona } }));
  }
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { createNavbar, initializeNavbar };
}
