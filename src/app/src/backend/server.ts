import express, { Request, Response } from 'express';
import cors from 'cors';

const app = express();
const PORT = 5000;

// Enable CORS for Electron frontend
app.use(cors());
app.use(express.json());

// Logging middleware
app.use((req: Request, res: Response, next) => {
    console.log(`${new Date().toISOString()} - ${req.method} ${req.path}`);
    next();
});

// Health check endpoint
app.get('/health', (req: Request, res: Response) => {
    res.json({
        status: 'ok',
        message: 'TypeScript backend is running'
    });
});

// Test endpoint for frontend
app.get('/api/test', (req: Request, res: Response) => {
    res.json({
        message: 'Hello from TypeScript backend!',
        status: 'success',
        data: {
            app: 'lemonade',
            version: '1.0.0'
        }
    });
});

// Get application information
app.get('/api/info', (req: Request, res: Response) => {
    res.json({
        app_name: 'Lemonade App',
        backend: 'TypeScript/Express',
        frontend: 'Electron',
        version: '1.0.0'
    });
});

// Start server
app.listen(PORT, '127.0.0.1', () => {
    console.log(`TypeScript backend server started on http://127.0.0.1:${PORT}`);
});

// Graceful shutdown
process.on('SIGTERM', () => {
    console.log('SIGTERM signal received: closing HTTP server');
    process.exit(0);
});

process.on('SIGINT', () => {
    console.log('SIGINT signal received: closing HTTP server');
    process.exit(0);
});

