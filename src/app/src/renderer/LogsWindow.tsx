import React from 'react';

interface LogsWindowProps {
  isVisible: boolean;
  height?: number;
}

const LogsWindow: React.FC<LogsWindowProps> = ({ isVisible, height }) => {
  const logsContent = `[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/health
[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 61808) =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 61808) =====
[Server] GET /api/v1/health - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/health
[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 59528) =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 59528) =====
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server] GET /api/v1/health - 200
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/health[Server PRE-ROUTE] GET /api/v1/models

[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 14548) =====
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 14548) =====
[Server] GET /api/v1/health - 200
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/health
[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 65964) =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 65964) =====
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server] GET /api/v1/health - 200
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/health
[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 55772) =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 55772) =====
[Server] GET /api/v1/health - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/health
[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 25632) =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 25632) =====
[Server] GET /api/v1/health - 200
[Server PRE-ROUTE] GET /api/v1/health
[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 30328) =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 30328) =====
[Server] GET /api/v1/health - 200
[Server PRE-ROUTE] GET /api/v1/models
[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====
[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====
[Server] GET /api/v1/models - 200
[Server PRE-ROUTE] GET /api/v1/health
[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: 65964) =====
[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: 65964) =====
[Server] GET /api/v1/health - 200`;

  if (!isVisible) return null;

  return (
    <div className="logs-window" style={height ? { height: `${height}px` } : undefined}>
      <div className="logs-header">
        <h3>LOGS</h3>
      </div>
      <div className="logs-content">
        <pre className="logs-text">{logsContent}</pre>
      </div>
    </div>
  );
};

export default LogsWindow;

