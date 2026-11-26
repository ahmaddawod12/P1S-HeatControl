export const STATUS_STALE_MS = 45_000;
export const COMMAND_TTL_MS = 30_000;
export const PAIRING_CODE_TTL_MS = 300_000;
export const MAX_COMMAND_SEQUENCE = 4_294_967_295;
export const MIN_RESTART_DROP_C = 0.5;
export const MAX_RESTART_DROP_C = 20;
export const FUNCTION_REGION = 'us-central1';
export const EMULATOR_PROJECT = 'demo-p1s-heat';
export const EMULATOR_DATABASE_URL = `https://${EMULATOR_PROJECT}-default-rtdb.firebaseio.com`;

export const commandLabels = {
  'heat.setTarget': 'Set target',
  'heat.setRestart': 'Set restart temperature',
  'heat.enable': 'Enable heat',
  'heat.disable': 'Disable heat',
  'heat.ackFault': 'Acknowledge fault',
  'sensor.setCalibration': 'Set calibration',
  'printer.monitorStart': 'Start Monitor P1S',
  'printer.monitorStop': 'Stop monitoring',
  'firmware.update': 'Start firmware update',
} as const;

export type CommandType = keyof typeof commandLabels;
export type CommandAction = { type: CommandType; payload: Record<string, number> };

export interface CommandRequest extends CommandAction {
  deviceUid: string;
  requestId: string;
  disableRevision: number;
  sessionId?: string;
}

export interface Command extends CommandAction {
  id: string;
  sequence: number;
  sessionId: string;
  issuedAt: number;
  expiresAt: number;
}

export interface CommandResult {
  id: string;
  status: 'applied' | 'rejected' | 'expired';
  reason: string;
  timestamp: number;
}

export interface CommandReceipt {
  id: string;
  expiresAt: number;
  // Receipts stored before command sessions were returned may omit this field.
  sessionId?: string;
}

export interface PairingReceipt {
  deviceUid: string;
  // Older callable deployments only return deviceUid.
  expiresAt?: number;
}

export interface LatestStatus {
  schemaVersion: 1;
  updatedAt: number;
  name: string;
  boardId?: string;
  firmwareVersion: string;
  state: string;
  fault: string;
  targetC: number;
  restartC?: number;
  calibrationOffsetC: number;
  temperatureValid: boolean;
  temperatureC?: number;
  heatEnabled: boolean;
  restorePending?: boolean;
  heaterOn: boolean;
  fanOn: boolean;
  printerConfigured: boolean;
  canMonitor: boolean;
  printerConnected: boolean;
  printerStateKnown: boolean;
  printerState: number;
  monitorPhase: number;
  canUpdate?: boolean;
  updatePhase: number;
  updateResult: number;
  updateReceived: number;
  updateTotal: number;
  heap?: { freeBytes: number; minimumFreeBytes: number };
  commandReady: boolean;
  sessionId: string;
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function canRequestFirmwareUpdate(latest: unknown): boolean {
  return isRecord(latest) && latest.canUpdate === true &&
    latest.heatEnabled === false && latest.heaterOn === false && latest.fanOn === false &&
    latest.restorePending !== true && latest.monitorPhase === 0 &&
    (latest.updatePhase === 0 || latest.updatePhase === 7 || latest.updatePhase === 8);
}

export function isUid(value: unknown): value is string {
  return typeof value === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(value);
}

export function isRequestId(value: unknown): value is string {
  return typeof value === 'string' && /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(value);
}

export function isCommandSession(value: unknown): value is string {
  return typeof value === 'string' && /^[0-9a-f]{32}$/.test(value);
}

export function isDisableRevision(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= MAX_COMMAND_SEQUENCE;
}

export function isFresh(updatedAt: unknown, now: number): updatedAt is number {
  return typeof updatedAt === 'number' && Number.isSafeInteger(updatedAt) &&
    updatedAt > 0 && updatedAt <= now + 5_000 && now - updatedAt < STATUS_STALE_MS;
}

export function isRestartForTarget(restartC: unknown, targetC: unknown): boolean {
  if (typeof restartC !== 'number' || typeof targetC !== 'number') return false;
  const drop = targetC - restartC;
  return drop >= MIN_RESTART_DROP_C && drop <= MAX_RESTART_DROP_C;
}

export function readCommandAction(type: unknown, payload: unknown): CommandAction | undefined {
  if (typeof type !== 'string' || !Object.hasOwn(commandLabels, type) || !isRecord(payload)) return;
  const commandType = type as CommandType;
  const keys = Object.keys(payload);
  if (commandType === 'heat.setTarget' || commandType === 'heat.setRestart') {
    const field = commandType === 'heat.setTarget' ? 'targetC' : 'restartC';
    const value = payload[field];
    const minimum = field === 'targetC' ? 0 : -MAX_RESTART_DROP_C;
    const maximum = field === 'targetC' ? 80 : 80 - MIN_RESTART_DROP_C;
    if (keys.length === 1 && typeof value === 'number' &&
        Number.isInteger(value * 2) && value >= minimum && value <= maximum) {
      return { type: commandType, payload: { [field]: value } };
    }
    return;
  }
  if (commandType === 'sensor.setCalibration') {
    const value = payload.calibrationOffsetC;
    if (keys.length === 1 && typeof value === 'number' && Number.isFinite(value) &&
        value >= -10 && value <= 10) {
      return { type: commandType, payload: { calibrationOffsetC: value } };
    }
    return;
  }
  if (keys.length === 0) return { type: commandType, payload: {} };
}

const statusStrings = ['name', 'firmwareVersion', 'state', 'fault', 'sessionId'] as const;
const statusNumbers = ['updatedAt', 'targetC', 'calibrationOffsetC', 'printerState',
  'monitorPhase', 'updatePhase', 'updateResult', 'updateReceived', 'updateTotal'] as const;
const statusFlags = ['temperatureValid', 'heatEnabled', 'heaterOn', 'fanOn', 'printerConfigured',
  'canMonitor', 'printerConnected', 'printerStateKnown', 'commandReady'] as const;

function isHeapBytes(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 0xffff_ffff;
}

function isHeapStatus(value: unknown): boolean {
  return isRecord(value) && isHeapBytes(value.freeBytes) && isHeapBytes(value.minimumFreeBytes) &&
    value.minimumFreeBytes <= value.freeBytes;
}

export function readLatest(value: unknown): LatestStatus | undefined {
  if (!isRecord(value) || value.schemaVersion !== 1 ||
      !statusStrings.every(key => typeof value[key] === 'string') ||
      !statusNumbers.every(key => typeof value[key] === 'number' && Number.isFinite(value[key])) ||
      !statusFlags.every(key => typeof value[key] === 'boolean') ||
      (value.boardId !== undefined && (typeof value.boardId !== 'string' || !/^[0-9A-F]{12}$/.test(value.boardId))) ||
      (value.restartC !== undefined && !isRestartForTarget(value.restartC, value.targetC)) ||
      (value.restorePending !== undefined && typeof value.restorePending !== 'boolean') ||
      (value.canUpdate !== undefined && typeof value.canUpdate !== 'boolean') ||
      (value.heap !== undefined && !isHeapStatus(value.heap)) ||
      (value.temperatureValid && (typeof value.temperatureC !== 'number' || !Number.isFinite(value.temperatureC)))) {
    return;
  }
  return value as unknown as LatestStatus;
}

export function readResult(value: unknown, id: string): CommandResult | undefined {
  if (!isRecord(value) || value.id !== id || typeof value.timestamp !== 'number' || !Number.isSafeInteger(value.timestamp) ||
      typeof value.reason !== 'string') return;
  if (value.status === 'applied' || value.status === 'rejected' || value.status === 'expired') {
    return { id, status: value.status, reason: value.reason, timestamp: value.timestamp };
  }
}
