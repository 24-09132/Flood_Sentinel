/**
 * Database & Authentication Helper for Vercel Serverless Functions
 * Supports Upstash Redis with graceful in-memory fallback
 */
const { Redis } = require('@upstash/redis');

let redis = null;
const redisUrl = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
const redisToken = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;

if (redisUrl && redisToken) {
  try {
    redis = new Redis({
      url: redisUrl,
      token: redisToken,
    });
  } catch (err) {
    console.warn('[DB] Upstash Redis initialization error:', err.message);
  }
}

// In-memory fallback telemetry (used only when Upstash Redis is not connected).
// Starts empty so the dashboard never shows made-up readings as "online".
let memoryTelemetry = {};

let memoryHistory = [];

/**
 * Validates request authorization using API Key
 * Supports environment variables: API_KEY, bridge_key, BRIDGE_KEY
 * Expected default key value: 'bridgingthegap'
 */
function isAuthorized(req) {
  const configuredKey = process.env.API_KEY || process.env.bridge_key || process.env.BRIDGE_KEY || 'bridgingthegap';
  
  // Extract key from headers (x-api-key, bridge_key, bridge-key) or query string
  const headerKey = req.headers['x-api-key'] || req.headers['bridge_key'] || req.headers['bridge-key'] || req.headers['x-bridge-key'];
  let queryKey = req.query && (req.query.key || req.query.bridge_key);
  if (!queryKey && req.url) {
    try {
      const u = new URL(req.url, 'http://localhost');
      queryKey = u.searchParams.get('key') || u.searchParams.get('bridge_key');
    } catch (e) {}
  }

  const providedKey = headerKey || queryKey;
  
  // Match configured key or standard default value 'bridgingthegap'
  return providedKey === configuredKey || providedKey === 'bridgingthegap' || (!process.env.API_KEY && !process.env.bridge_key && !providedKey);
}

function setCorsHeaders(res) {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'x-api-key, bridge_key, bridge-key, x-bridge-key, Content-Type');
}

module.exports = {
  redis,
  memoryTelemetry,
  memoryHistory,
  isAuthorized,
  setCorsHeaders
};
