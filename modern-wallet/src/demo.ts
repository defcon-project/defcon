// The browser-only demo dataset. When the app runs outside Tauri (plain
// `npm run dev`), api.ts routes every command here, so the whole interface
// stays viewable and testable without a node. The DEMO badge in the top bar is
// derived from the same switch, so this mode can never masquerade as live.

const nowSec = Math.floor(Date.now() / 1000)

export const demoResponses: Record<string, unknown> = {
  get_connection: {
    connected: true,
    chain: 'demo',
    datadir: '(browser demo — no Core connection)',
    managed: false,
    wallet: 'First',
  },
  node_status: {
    blockchain: { chain: 'demo', blocks: 124029, headers: 124029, verificationprogress: 1 },
    network: { subversion: '/Defcon Core:22.1.5/', connections: 12 },
    chainlock: { height: 124028, blockhash: '00'.repeat(32) },
  },
  wallet_summary: {
    balances: {
      mine: { trusted: 49329976.01, untrusted_pending: 0, immature: 5500 },
      watchonly: { trusted: 1179999.98, untrusted_pending: 0, immature: 0 },
    },
    info: { walletname: 'First', unlocked_until: 0 },
    staking: {
      '0': {
        name: 'First',
        staking: 'true',
        errors: '',
        weight: 785000000000000,
        netstakeweight: 15400000000000000,
        expectedtime: 9660,
        excluded: { immature: 5500, too_small: 800.94 },
      },
    },
  },
  list_transactions: [
    { txid: 'a1'.repeat(32), category: 'receive', amount: 500, confirmations: 38, time: nowSec - 3600, address: 'DGR5G59z7hCh4Trq3ZjvuDgbsecbUUF5UM', label: '' },
    { txid: 'b2'.repeat(32), category: 'receive', amount: 10000, confirmations: 104, time: nowSec - 13000, address: 'DHSx9Fq7JtB3WvK6e2YnR8mPa4LcN1uQzd', label: 'masternode reward' },
    { txid: 'c3'.repeat(32), category: 'send', amount: -1200.5, confirmations: 210, time: nowSec - 86000, address: 'D7K4JvQcVma1R8ZyA6nW3Xf9uH5pEe2TsB', label: 'Hetzner' },
    { txid: 'd4'.repeat(32), category: 'stake', amount: 500, confirmations: 350, time: nowSec - 172000, address: 'DGR5G59z7hCh4Trq3ZjvuDgbsecbUUF5UM', label: '' },
    { txid: 'e5'.repeat(32), category: 'receive', amount: 250000, confirmations: 1200, time: nowSec - 400000, address: 'DQm8N3vR6xT1cY4bA7sW2eK9uJ5hL0pGzf', label: 'exchange' },
  ],
  masternode_list: [
    { proTxHash: 'f0'.repeat(32), address: '203.0.113.10:9999', payee: 'DP1demo', status: 'ENABLED', pospenaltyscore: 0, lastpaidblock: 123950, mine: true },
    { proTxHash: 'f1'.repeat(32), address: '203.0.113.11:9999', payee: 'DP1demo', status: 'ENABLED', pospenaltyscore: 0, lastpaidblock: 123890, mine: true },
    { proTxHash: 'f2'.repeat(32), address: '203.0.113.12:9999', payee: 'DP1other', status: 'POSE_BANNED', pospenaltyscore: 100, lastpaidblock: 123500, mine: false },
    { proTxHash: 'f3'.repeat(32), address: '203.0.113.13:9999', payee: 'DP1other', status: 'ENABLED', pospenaltyscore: 0, lastpaidblock: 124000, mine: false },
  ],
  governance_list: {
    'g1': {
      DataString: '{"name":"core-development-q4","payment_amount":"120000","url":"https://example.invalid/dev"}',
      Hash: 'aa'.repeat(32),
      FundingResult: { YesCount: 74, NoCount: 6, AbstainCount: 1 },
    },
    'g2': {
      DataString: '{"name":"community-outreach","payment_amount":"40000","url":"https://example.invalid/out"}',
      Hash: 'bb'.repeat(32),
      FundingResult: { YesCount: 41, NoCount: 22, AbstainCount: 3 },
    },
  },
  coinjoin_info: {
    enabled: true,
    running: false,
    rounds: 4,
    max_amount: 100000,
    keys_left: 999,
    sessions: [],
  },
  get_new_address: 'DGR5G59z7hCh4Trq3ZjvuDgbsecbUUF5UM',
  list_receive_addresses: [
    { address: 'DGR5G59z7hCh4Trq3ZjvuDgbsecbUUF5UM', label: 'default', amount: 250500 },
    { address: 'DQm8N3vR6xT1cY4bA7sW2eK9uJ5hL0pGzf', label: 'exchange', amount: 250000 },
  ],
  validate_address: { isvalid: true },
  send_to_address: 'deadbeef'.repeat(8),
  wallet_lock: null,
  wallet_unlock: null,
  gui_get_mode: 'modern',
  gui_set_mode: 'modern',
  preview_defcond_args: ['-server=1', '-daemon=0'],
  list_wallets: ['First'],
  select_wallet: null,
  connect: {
    connected: true,
    chain: 'demo',
    datadir: '(browser demo — no Core connection)',
    managed: false,
    wallet: 'First',
  },
}

export async function demoInvoke<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  await new Promise((resolve) => setTimeout(resolve, 120))
  if (command === 'gui_set_mode') return (args?.mode ?? 'modern') as T
  if (command === 'console_execute') {
    if (args?.command === 'help')
      return ['getblockchaininfo', 'getstakinginfo', 'getbalances', 'getnetworkinfo',
        'listwallets', 'listtransactions', 'validateaddress', 'masternodelist', 'help',
      ].join(String.fromCharCode(10)) as T
    return { demo: true, echo: args?.command ?? '', note: 'browser demo: no node behind this console' } as T
  }
  if (!(command in demoResponses)) throw new Error(`demo mode has no handler for ${command}`)
  return structuredClone(demoResponses[command]) as T
}
