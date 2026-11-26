import {
  COMMAND_TTL_MS, MAX_COMMAND_SEQUENCE, canRequestFirmwareUpdate, isCommandSession, isDisableRevision, isFresh, isRecord,
  isRequestId, isRestartForTarget, isUid, readCommandAction, readResult, type Command, type CommandAction, type CommandReceipt,
  type CommandRequest,
} from './contract.js';

const QUEUE_LIMIT = 8;
const RECEIPT_LIMIT = 256;
const RECEIPT_LIFETIME_MS = 24 * 60 * 60 * 1_000;
const UNBOUND_DISABLE_SESSION = '00000000000000000000000000000000';

type CommandError = 'offline' | 'queue-full' | 'reused-request' | 'sequence-exhausted' | 'superseded' | 'stale-session' |
  'restart-range' | 'update-unavailable';
export type Submission =
  | { receipt: CommandReceipt; write?: Record<string, unknown> }
  | { error: CommandError };

export function readCommandRequest(value: unknown): CommandRequest | undefined {
  if (!isRecord(value) || !isUid(value.deviceUid) || !isRequestId(value.requestId)) return;
  const includesRevision = Object.hasOwn(value, 'disableRevision');
  const disableRevision = includesRevision ? value.disableRevision : 0;
  const includesSession = Object.hasOwn(value, 'sessionId');
  const sessionId = includesSession ? value.sessionId : undefined;
  if (Object.keys(value).length !== 4 + Number(includesRevision) + Number(includesSession) ||
      !isDisableRevision(disableRevision) || (includesSession && !isCommandSession(sessionId))) return;
  const action = readCommandAction(value.type, value.payload);
  if (action) {
    return { ...action, deviceUid: value.deviceUid, requestId: value.requestId,
      disableRevision, sessionId: typeof sessionId === 'string' ? sessionId : undefined };
  }
}

function recordEntries(value: unknown): [string, Record<string, unknown>][] {
  if (!isRecord(value)) return [];
  return Object.entries(value).filter((item): item is [string, Record<string, unknown>] => isRecord(item[1]));
}

function fingerprint(action: CommandAction) {
  return JSON.stringify([action.type, action.payload]);
}

export function submitCommand(current: unknown, request: CommandRequest, now: number): Submission {
  const device = isRecord(current) ? current : {};
  const disable = request.type === 'heat.disable';
  const receipts = recordEntries(device.commandReceipts);
  const previous = receipts.find(([id, receipt]) =>
    id === request.requestId && typeof receipt.retainUntil === 'number' && receipt.retainUntil > now)?.[1];
  if (previous) {
    if (previous.fingerprint !== fingerprint(request) || typeof previous.expiresAt !== 'number' ||
        (!disable && request.sessionId && isCommandSession(previous.sessionId) &&
          request.sessionId !== previous.sessionId)) {
      return { error: 'reused-request' };
    }
    const receipt: CommandReceipt = { id: request.requestId, expiresAt: previous.expiresAt };
    if (isCommandSession(previous.sessionId)) receipt.sessionId = previous.sessionId;
    return { receipt };
  }

  const disableRevision = device.disableRevision ?? 0;
  // The revision comes from the snapshot used to submit the request. This also
  // fences commands that arrive after Disable and have never been queued.
  if (!disable && request.disableRevision !== disableRevision) return { error: 'superseded' };

  const latest = device.latest;
  if (!isRecord(latest) || latest.schemaVersion !== 1 || !isFresh(latest.updatedAt, now)) {
    return { error: 'offline' };
  }
  const readySession = latest.commandReady === true && isCommandSession(latest.sessionId) ? latest.sessionId : undefined;
  if (!disable && !readySession) return { error: 'offline' };
  // Older clients can recover receipts and send Disable. New heating actions
  // require the session the dashboard was showing when the user submitted them.
  if (!disable && request.sessionId !== readySession) return { error: 'stale-session' };
  if (request.type === 'firmware.update' && !canRequestFirmwareUpdate(latest)) {
    return { error: 'update-unavailable' };
  }
  if (request.type === 'heat.setRestart' &&
      (latest.targetC === 0 || !isRestartForTarget(latest.restartC, latest.targetC) ||
       !isRestartForTarget(request.payload.restartC, latest.targetC))) {
    return { error: 'restart-range' };
  }
  const resultRecords = isRecord(device.results) ? device.results : {};
  // Keep an outstanding Disable when the device publishes its new session after a reconnect.
  const pending = recordEntries(device.commands).filter(([id, command]) =>
    (command.sessionId === readySession || command.type === 'heat.disable') && typeof command.expiresAt === 'number' &&
    command.expiresAt > now && !readResult(resultRecords[id], id));
  if (!disable && pending.length >= QUEUE_LIMIT - 1) return { error: 'queue-full' };

  const lastSequence = device.commandSequence ?? 0;
  if (typeof lastSequence !== 'number' || !Number.isInteger(lastSequence) ||
      lastSequence < 0 || lastSequence >= MAX_COMMAND_SEQUENCE) return { error: 'sequence-exhausted' };

  const command: Command = {
    id: request.requestId,
    type: request.type,
    payload: request.payload,
    sessionId: readySession ?? UNBOUND_DISABLE_SESSION,
    sequence: lastSequence + 1,
    issuedAt: now,
    expiresAt: now + COMMAND_TTL_MS,
  };
  // A new Disable takes precedence over every older outstanding request.
  const queue = disable ? {} : Object.fromEntries(pending);
  const keptReceipts = Object.fromEntries(receipts
    .filter(([, receipt]) => typeof receipt.retainUntil === 'number' && receipt.retainUntil > now)
    .sort(([, a], [, b]) => Number(b.retainUntil) - Number(a.retainUntil))
    .slice(0, RECEIPT_LIMIT - 1));
  const results = Object.fromEntries(recordEntries(resultRecords)
    .filter(([id]) => Object.hasOwn(keptReceipts, id)));
  return {
    receipt: { id: command.id, expiresAt: command.expiresAt, sessionId: command.sessionId },
    write: {
      ...device,
      commandSequence: command.sequence,
      disableRevision: disable ? command.sequence : disableRevision,
      commands: { ...queue, [command.id]: command },
      commandReceipts: {
        ...keptReceipts,
        [command.id]: {
          expiresAt: command.expiresAt,
          sessionId: command.sessionId,
          retainUntil: now + RECEIPT_LIFETIME_MS,
          fingerprint: fingerprint(request),
        },
      },
      results,
    },
  };
}
