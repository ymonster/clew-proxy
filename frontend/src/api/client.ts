import type { AutoRule, ProcessInfo, ProxyStatus, Stats, TcpConnection, NetworkConnection, ProxyGroup, GroupInUseError, ProxyTestResult } from './types'

const BASE = '/api'
const TOKEN_STORAGE_KEY = 'clew_token'

let authToken: string | null = null
let authInitialized = false

export async function initAuth(): Promise<void> {
  try {
    const res = await fetch(`${BASE}/bootstrap`)
    if (!res.ok) return
    const data = await res.json() as { auth_enabled?: boolean }
    if (!data.auth_enabled) return
    let stored = localStorage.getItem(TOKEN_STORAGE_KEY)
    if (!stored) {
      stored = window.prompt('Clew API token required (from clew.json auth.token):') || ''
      if (stored) localStorage.setItem(TOKEN_STORAGE_KEY, stored)
    }
    authToken = stored || null
  } finally {
    authInitialized = true
  }
}

export function getAuthToken(): string | null {
  return authToken
}

function authHeaders(): Record<string, string> {
  return authToken ? { 'Authorization': `Bearer ${authToken}` } : {}
}

function mergeHeaders(init?: RequestInit): HeadersInit {
  return {
    'Content-Type': 'application/json',
    ...authHeaders(),
    ...(init?.headers as Record<string, string> | undefined),
  }
}

async function request<T>(url: string, options?: RequestInit): Promise<T> {
  if (!authInitialized) await initAuth()
  const res = await fetch(`${BASE}${url}`, {
    ...options,
    headers: mergeHeaders(options),
  })
  if (res.status === 401) {
    localStorage.removeItem(TOKEN_STORAGE_KEY)
    authToken = null
    throw new Error('API error 401: unauthorized. Set auth.token in clew.json or clear localStorage and reload.')
  }
  if (!res.ok) {
    const text = await res.text().catch(() => res.statusText)
    throw new Error(`API error ${res.status}: ${text}`)
  }
  const text = await res.text()
  if (!text) {
    throw new Error(`API error: expected JSON body for ${url}, got empty response`)
  }
  return JSON.parse(text) as T
}

async function requestVoid(url: string, options?: RequestInit): Promise<void> {
  if (!authInitialized) await initAuth()
  const res = await fetch(`${BASE}${url}`, {
    ...options,
    headers: mergeHeaders(options),
  })
  if (res.status === 401) {
    localStorage.removeItem(TOKEN_STORAGE_KEY)
    authToken = null
    throw new Error('API error 401: unauthorized.')
  }
  if (!res.ok) {
    const text = await res.text().catch(() => res.statusText)
    throw new Error(`API error ${res.status}: ${text}`)
  }
}

// -- Processes --

export function getProcesses(): Promise<ProcessInfo[]> {
  return request<ProcessInfo[]>('/processes')
}

export function getProcessDetail(pid: number): Promise<ProcessInfo> {
  return request<ProcessInfo>(`/processes/${pid}/detail`)
}

// -- Hijack --

export function hijackProcess(pid: number, tree = true, groupId = 0): Promise<void> {
  return requestVoid(`/hijack/${pid}`, {
    method: 'POST',
    body: JSON.stringify({ tree, group_id: groupId }),
  })
}

export function unhijackProcess(pid: number, tree = false): Promise<void> {
  const query = tree ? '?tree=true' : ''
  return requestVoid(`/hijack/${pid}${query}`, { method: 'DELETE' })
}

export function getHijackedProcesses(): Promise<ProcessInfo[]> {
  return request<ProcessInfo[]>('/hijack')
}

export function batchHijack(pids: number[], action: 'hijack' | 'unhijack', groupId = 0): Promise<void> {
  return requestVoid('/hijack/batch', {
    method: 'POST',
    body: JSON.stringify({ pids, action, group_id: groupId }),
  })
}

// -- TCP Connections --

export function getTcpConnections(pid?: number): Promise<TcpConnection[]> {
  const query = pid != null ? `?pid=${pid}` : ''
  return request<TcpConnection[]>(`/tcp${query}`)
}

export function getUdpConnections(pid?: number): Promise<TcpConnection[]> {
  const query = pid != null ? `?pid=${pid}` : ''
  return request<TcpConnection[]>(`/udp${query}`)
}

export async function getNetworkConnections(pid?: number): Promise<NetworkConnection[]> {
  const [tcp, udp] = await Promise.all([
    getTcpConnections(pid),
    getUdpConnections(pid),
  ])
  return [
    ...tcp.map(c => ({ ...c, protocol: 'TCP' as const })),
    ...udp.map(c => ({ ...c, protocol: 'UDP' as const })),
  ]
}

// -- Auto Rules --

export function getAutoRules(): Promise<AutoRule[]> {
  return request<AutoRule[]>('/auto-rules')
}

export function createAutoRule(rule: Omit<AutoRule, 'id'>): Promise<AutoRule> {
  return request<AutoRule>('/auto-rules', {
    method: 'POST',
    body: JSON.stringify(rule),
  })
}

export function updateAutoRule(id: string, rule: Partial<AutoRule>): Promise<AutoRule> {
  return request<AutoRule>(`/auto-rules/${id}`, {
    method: 'PUT',
    body: JSON.stringify(rule),
  })
}

export function deleteAutoRule(id: string): Promise<void> {
  return requestVoid(`/auto-rules/${id}`, { method: 'DELETE' })
}

export function excludePid(ruleId: string, pid: number): Promise<void> {
  return requestVoid(`/auto-rules/${ruleId}/exclude/${pid}`, { method: 'POST' })
}

export function unexcludePid(ruleId: string, pid: number): Promise<void> {
  return requestVoid(`/auto-rules/${ruleId}/exclude/${pid}`, { method: 'DELETE' })
}

// -- Config --

export function getConfig(): Promise<Record<string, unknown>> {
  return request<Record<string, unknown>>('/config')
}

export function updateConfig(config: Record<string, unknown>): Promise<void> {
  return requestVoid('/config', {
    method: 'PUT',
    body: JSON.stringify(config),
  })
}

// -- Proxy --

export function getProxyStatus(): Promise<ProxyStatus> {
  return request<ProxyStatus>('/proxy/status')
}

export function startProxy(): Promise<void> {
  return requestVoid('/proxy/start', { method: 'POST' })
}

export function stopProxy(): Promise<void> {
  return requestVoid('/proxy/stop', { method: 'POST' })
}

// -- Proxy Groups --

export function getProxyGroups(): Promise<ProxyGroup[]> {
  return request<ProxyGroup[]>('/proxy-groups')
}

export function createProxyGroup(group: Omit<ProxyGroup, 'id'>): Promise<ProxyGroup> {
  return request<ProxyGroup>('/proxy-groups', {
    method: 'POST',
    body: JSON.stringify(group),
  })
}

export function updateProxyGroup(id: number, group: Partial<ProxyGroup>): Promise<void> {
  return requestVoid(`/proxy-groups/${id}`, {
    method: 'PUT',
    body: JSON.stringify(group),
  })
}

export async function deleteProxyGroup(id: number): Promise<{ success: boolean } | GroupInUseError> {
  if (!authInitialized) await initAuth()
  const res = await fetch(`${BASE}/proxy-groups/${id}`, {
    method: 'DELETE',
    headers: mergeHeaders(),
  })
  const text = await res.text()
  const data = text ? JSON.parse(text) : { success: true }
  if (res.status === 409) return data as GroupInUseError
  if (!res.ok) throw new Error(`API error ${res.status}: ${JSON.stringify(data)}`)
  return data
}

export function migrateProxyGroup(id: number, targetGroupId: number): Promise<void> {
  return requestVoid(`/proxy-groups/${id}/migrate`, {
    method: 'POST',
    body: JSON.stringify({ target_group_id: targetGroupId }),
  })
}

export function testProxyGroup(id: number): Promise<ProxyTestResult> {
  return request<ProxyTestResult>(`/proxy-groups/${id}/test`, {
    method: 'POST',
  })
}

// -- Stats --

export function getStats(): Promise<Stats> {
  return request<Stats>('/stats')
}

// -- Shell --

export function revealFile(path: string): Promise<void> {
  return requestVoid('/shell/reveal', {
    method: 'POST',
    body: JSON.stringify({ path }),
  })
}

export function browseExe(): Promise<{ cancelled?: boolean; path?: string; dir?: string; name?: string }> {
  return request<{ cancelled?: boolean; path?: string; dir?: string; name?: string }>('/shell/browse-exe', {
    method: 'POST',
  })
}
