# Lemonade App

A cross-platform desktop application built with Electron and TypeScript backend for lemonade management.

## Features

- 🖥️ Cross-platform support (Windows, macOS, Linux)
- ⚡ Electron frontend with modern UI
- 🔷 TypeScript/Express backend for snappy performance
- 📦 Automated installer builds via GitHub Actions
- 🔄 Real-time communication between frontend and backend

## Prerequisites

Before you begin, ensure you have the following installed:

- **Node.js** (v18 or higher)
- **npm** (comes with Node.js)

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/lemonade-sdk/lemonade-next.git
cd lemonade-next
```

### 2. Install dependencies

```bash
npm install
```

## Development

### Running the Application

To run the application in development mode:

```bash
npm start
```

This will:
1. Build the TypeScript backend
2. Start the backend server on `http://localhost:5000`
3. Launch the Electron application

### Development Scripts

```bash
# Start the full application
npm start

# Run with developer tools open (recommended for debugging)
npm run dev

# Run only the TypeScript backend (for testing)
npm run backend

# Build TypeScript backend only
npm run build:backend

# Watch mode for backend development
npm run watch:backend
```

## Building

### Build for your current platform

```bash
npm run build
```

### Build for specific platforms

```bash
# Windows
npm run build:win

# macOS
npm run build:mac

# Linux
npm run build:linux
```

The built installers will be in the `dist-app` folder.

## Project Structure

```
lemonade/
├── .github/
│   └── workflows/
│       └── build.yml          # GitHub Actions workflow for automated builds
├── src/
│   └── backend/
│       └── server.ts         # TypeScript/Express backend server
├── dist/                     # Compiled TypeScript output
├── index.html                # Main HTML file
├── main.js                   # Electron main process
├── preload.js                # Electron preload script
├── renderer.js               # Renderer process JavaScript
├── styles.css                # Application styles
├── tsconfig.json             # TypeScript configuration
├── package.json              # Node.js dependencies and scripts
└── README.md                 # This file
```

## GitHub Actions CI/CD

This project includes a GitHub Actions workflow that automatically builds installers for Windows, macOS, and Linux.

### Triggering Builds

The workflow runs on:

1. **Push to tags** starting with 'v' (e.g., `v1.0.0`)
2. **Pull requests** to main/master branches
3. **Manual trigger** via GitHub Actions UI

### Creating a Release

To create a new release with installers:

```bash
# Tag your commit
git tag v1.0.0

# Push the tag
git push origin v1.0.0
```

The workflow will automatically:
- Build installers for Windows (.exe), macOS (.dmg), and Linux (.AppImage)
- Upload artifacts
- Create a GitHub release with all installers attached

### Downloading Artifacts

- **For PRs and manual runs**: Download artifacts from the GitHub Actions run page
- **For tagged releases**: Download installers from the GitHub Releases page

## API Endpoints

The TypeScript backend provides the following endpoints:

- `GET /health` - Health check endpoint
- `GET /api/test` - Test endpoint for frontend connectivity
- `GET /api/info` - Application information

## Customization

### Adding New Backend Endpoints

Edit `src/backend/server.ts` and add new Express routes:

```typescript
app.get('/api/your-endpoint', (req: Request, res: Response) => {
    res.json({ message: 'Your response' });
});
```

### Modifying the Frontend

- **HTML**: Edit `index.html`
- **Styles**: Edit `styles.css`
- **JavaScript**: Edit `renderer.js`

### Changing App Configuration

Edit `package.json` under the `build` section to customize:
- App name
- App ID
- Icons
- Installer settings

## Troubleshooting

If you encounter any issues, please refer to the [TROUBLESHOOTING.md](TROUBLESHOOTING.md) guide for detailed solutions.

### Quick Fixes

**Backend not connecting:**
```bash
# Check Node.js installation
node --version

# Rebuild backend
npm run build:backend

# Test backend separately
npm run backend
```

**Build fails:**
```bash
# Reinstall dependencies
rm -rf node_modules
npm ci

# Clean and rebuild
rm -rf dist dist-app
npm run build
```

**Port 5000 already in use:**

Check what's using the port and stop it, or change the port in both `src/backend/server.ts` and `renderer.js`.

## License

MIT

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

