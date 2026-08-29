// The one place the UI talks to the outside. Inside Tauri every call is a
// typed command on the Rust bridge; in a plain browser it is the demo dataset.
// Nothing else in the React tree may import @tauri-apps/* directly, so the
// security boundary stays visible in one file.

import type {
  Balances,
  CoinJoinInfo,
  ConnectionInfo,
  GuiMode,
  MasternodeEntry,
  NodeStatus,
  Proposal,
  ReceiveAddress,
  StakingInfo,
  TxItem,
  TxKind,
  WalletSummary,
} from './types'
import { demoInvoke } from './demo'

export const isTauri = typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window

async function invoke<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  if (!isTauri) return demoInvoke<T>(command, args)
  const { invoke } = await import('@tauri-apps/api/core')
  return invoke<T>(command, args)
}

// ---------------------------------------------------------------- connection

export const getConnection = () => invoke<ConnectionInfo>('get_connection')
export const connect = () => invoke<ConnectionInfo>('connect')
export const startNode = (defcondPath?: string) =>
  invoke<ConnectionInfo>('start_node', { defcondPath: defcondPath ?? null })
export const stopNode = (force = false) => invoke<ConnectionInfo>('stop_node', { force })
export const setChain = (chain: string, devnetName?: string, datadir?: string) =>
  invoke<ConnectionInfo>('set_chain', { chain, devnetName: devnetName ?? null, datadir: datadir ?? null })
export const listWallets = () => invoke<string[]>('list_wallets')
export const selectWallet = (name: string) => invoke<void>('select_wallet', { name })

// ---------------------------------------------------------------- node views

export const getNodeStatus = async (): Promise<NodeStatus> => {
  const raw = await invoke<any>('node_status')
  return {
    blockchain: {
      chain: raw.blockchain?.chain ?? '?',
      blocks: raw.blockchain?.blocks ?? 0,
      headers: raw.blockchain?.headers ?? 0,
      verificationprogress: raw.blockchain?.verificationprogress ?? 0,
    },
    network: {
      subversion: raw.network?.subversion ?? '',
      connections: raw.network?.connections ?? 0,
    },
    chainlock:
      raw.chainlock && typeof raw.chainlock.height === 'number'
        ? { height: raw.chainlock.height, blockhash: raw.chainlock.blockhash ?? '' }
        : null,
  }
}

const mapBalances = (raw: any): Balances => ({
  trusted: raw?.mine?.trusted ?? 0,
  pending: raw?.mine?.untrusted_pending ?? 0,
  immature: raw?.mine?.immature ?? 0,
  watchonly: raw?.watchonly?.trusted ?? 0,
})

// getstakinginfo on this fork answers per wallet, keyed "0", "1", … — take
// the first entry; weights are satoshi and become DFCN here.
const mapStaking = (raw: any): StakingInfo | null => {
  if (!raw || typeof raw !== 'object') return null
  const first = Object.values(raw)[0] as any
  if (!first || typeof first !== 'object') return null
  return {
    staking: first.staking === 'true' || first.staking === true,
    errors: first.errors ?? '',
    weight: (first.weight ?? 0) / 1e8,
    netstakeweight: (first.netstakeweight ?? 0) / 1e8,
    expectedtime: typeof first.expectedtime === 'number' ? first.expectedtime : null,
    excluded: first.excluded ?? null,
  }
}

export const getWalletSummary = async (): Promise<WalletSummary> => {
  const raw = await invoke<any>('wallet_summary')
  return {
    walletName: raw.info?.walletname ?? '',
    encrypted: typeof raw.info?.unlocked_until === 'number',
    unlockedUntil: typeof raw.info?.unlocked_until === 'number' ? raw.info.unlocked_until : null,
    balances: mapBalances(raw.balances),
    staking: mapStaking(raw.staking),
  }
}

const kindOf = (category: string, amount: number): TxKind => {
  if (category === 'send') return 'sent'
  if (category === 'receive') return 'received'
  if (category === 'stake' || category === 'generate' || category === 'immature') return 'staking'
  return amount >= 0 ? 'received' : 'other'
}

export const getTransactions = async (count = 100, skip = 0): Promise<TxItem[]> => {
  const raw = await invoke<any[]>('list_transactions', { count, skip })
  return raw
    .map((t): TxItem => ({
      txid: t.txid ?? '',
      kind: kindOf(t.category ?? '', t.amount ?? 0),
      categoryRaw: t.category ?? '',
      label: t.label ?? '',
      address: t.address ?? '',
      amount: t.amount ?? 0,
      confirmations: t.confirmations ?? 0,
      time: t.time ?? 0,
    }))
    .reverse() // node lists oldest first; the UI wants newest on top
}

export const getMasternodes = async (): Promise<MasternodeEntry[]> => {
  const raw = await invoke<any[]>('masternode_list')
  return (raw ?? []).map((v) => ({
    proTxHash: v.proTxHash ?? '',
    address: v.address ?? '',
    payee: v.payee ?? '',
    status: v.status ?? '?',
    posePenalty: v.pospenaltyscore ?? v.posepenalty ?? 0,
    lastPaidHeight: v.lastpaidblock ?? 0,
    mine: v.mine === true,
  }))
}

export const getProposals = async (): Promise<Proposal[]> => {
  const raw = await invoke<Record<string, any>>('governance_list')
  return Object.entries(raw ?? {}).map(([key, v]) => {
    let name = key.slice(0, 12)
    let url = ''
    let paymentAmount = ''
    try {
      const parsed = JSON.parse(v.DataString ?? '{}')
      const data = Array.isArray(parsed) ? parsed[0]?.[1] ?? parsed[0] ?? {} : parsed
      name = data.name ?? name
      url = data.url ?? ''
      paymentAmount = String(data.payment_amount ?? '')
    } catch {
      // an unparseable proposal still renders, by hash
    }
    const votes = v.FundingResult ?? v
    return {
      hash: v.Hash ?? key,
      name,
      url,
      paymentAmount,
      yes: votes.YesCount ?? 0,
      no: votes.NoCount ?? 0,
      abstain: votes.AbstainCount ?? 0,
    }
  })
}

export const getCoinJoinInfo = async (): Promise<CoinJoinInfo> => {
  const raw = await invoke<any>('coinjoin_info')
  return {
    enabled: raw.enabled ?? false,
    running: raw.running ?? false,
    rounds: raw.rounds ?? 0,
    amount: raw.max_amount ?? raw.amount ?? 0,
    keysLeft: raw.keys_left ?? raw.keysleft ?? 0,
    sessions: Array.isArray(raw.sessions) ? raw.sessions.length : 0,
  }
}

// ---------------------------------------------------------------- actions

export const getNewAddress = (label: string) => invoke<string>('get_new_address', { label })

export const getReceiveAddresses = async (): Promise<ReceiveAddress[]> => {
  const raw = await invoke<any[]>('list_receive_addresses')
  return (raw ?? [])
    .map((r) => ({ address: r.address ?? '', label: r.label ?? '', amount: r.amount ?? 0 }))
    .filter((r) => r.address)
}

export const validateAddress = async (address: string): Promise<boolean> => {
  const raw = await invoke<any>('validate_address', { address })
  return raw?.isvalid === true
}

export const sendToAddress = (address: string, amount: string, label?: string) =>
  invoke<string>('send_to_address', { address, amount, label: label ?? null })

export const consoleExecute = (command: string) => invoke<unknown>('console_execute', { command })

export const walletLock = () => invoke<void>('wallet_lock')
export const walletUnlock = (passphrase: string, timeoutSeconds: number) =>
  invoke<void>('wallet_unlock', { passphrase, timeoutSeconds })

export const getGuiMode = () => invoke<GuiMode>('gui_get_mode')
export const setGuiMode = (mode: GuiMode) => invoke<GuiMode>('gui_set_mode', { mode })
export const previewDefcondArgs = () => invoke<string[]>('preview_defcond_args')

export const copyText = async (text: string) => {
  if (isTauri) {
    const { writeText } = await import('@tauri-apps/plugin-clipboard-manager')
    await writeText(text)
  } else {
    await navigator.clipboard?.writeText(text)
  }
}
