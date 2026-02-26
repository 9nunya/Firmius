import type { NextConfig } from "next";

const isDev = process.env.NODE_ENV === "development";
const API_BACKEND_URL = process.env.API_BACKEND_URL ?? "http://localhost:9174";

// Build timestamp for cache busting
const BUILD_TIMESTAMP = Date.now().toString();

const nextConfig: NextConfig = {
  ...(isDev ? {} : { output: 'export' }),
  trailingSlash: true,
  outputFileTracingRoot: process.cwd(),
  typescript: {
    ignoreBuildErrors: true,
  },
  eslint: {
    ignoreDuringBuilds: true,
  },
  images: {
    unoptimized: true,
  },
  env: {
    ...(isDev ? { NEXT_PUBLIC_API_BACKEND_URL: API_BACKEND_URL } : {}),
    NEXT_PUBLIC_BUILD_TIMESTAMP: BUILD_TIMESTAMP,
  },
  experimental: {
    ppr: false,
  },
  ...(isDev
    ? {
        rewrites: async () => [
          {
            source: "/api/:path*",
            destination: `${API_BACKEND_URL}/api/:path*`,
          },
        ],
      }
    : {}),
};

export default nextConfig;
