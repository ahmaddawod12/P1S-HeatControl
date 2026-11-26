import { initializeApp, type FirebaseOptions } from 'firebase/app';
import { connectAuthEmulator, getAuth } from 'firebase/auth';
import { connectDatabaseEmulator, getDatabase } from 'firebase/database';
import { connectFunctionsEmulator, getFunctions, httpsCallable } from 'firebase/functions';
import {
  EMULATOR_DATABASE_URL, EMULATOR_PROJECT, FUNCTION_REGION, type CommandReceipt, type CommandRequest, type PairingReceipt,
} from '../../functions/src/contract.js';

// The function allows 15 seconds of execution, plus cold start and network time.
const CALLABLE_TIMEOUT_MS = 30_000;

export function connectCloud() {
  const emulator = import.meta.env.MODE === 'emulator';
  const config: FirebaseOptions = emulator ? {
    projectId: EMULATOR_PROJECT,
    apiKey: 'demo-api-key',
    authDomain: 'demo-p1s-heat.firebaseapp.com',
    databaseURL: EMULATOR_DATABASE_URL,
    appId: '1:123456789:web:0000000000000000',
  } : {
    projectId: import.meta.env.VITE_FIREBASE_PROJECT_ID,
    apiKey: import.meta.env.VITE_FIREBASE_API_KEY,
    authDomain: import.meta.env.VITE_FIREBASE_AUTH_DOMAIN,
    databaseURL: import.meta.env.VITE_FIREBASE_DATABASE_URL,
    appId: import.meta.env.VITE_FIREBASE_APP_ID,
  };
  if (Object.values(config).some(value => typeof value !== 'string' || !value.trim())) {
    throw new Error('Firebase configuration is required. Set the web app values in cloud/.env.');
  }

  const app = initializeApp(config);
  const auth = getAuth(app);
  const database = getDatabase(app);
  const functions = getFunctions(app, FUNCTION_REGION);
  if (emulator) {
    connectAuthEmulator(auth, 'http://127.0.0.1:9099', { disableWarnings: true });
    connectDatabaseEmulator(database, '127.0.0.1', Number(import.meta.env.VITE_FIREBASE_DATABASE_EMULATOR_PORT ?? 9000));
    connectFunctionsEmulator(functions, '127.0.0.1', 5001);
  }
  return {
    auth,
    database,
    claim: httpsCallable<{ code: string }, PairingReceipt>(functions, 'claimPairing', { timeout: CALLABLE_TIMEOUT_MS }),
    issue: httpsCallable<CommandRequest, CommandReceipt>(
      functions, 'issueCommand', { timeout: CALLABLE_TIMEOUT_MS }),
  };
}
