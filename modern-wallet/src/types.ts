// Shapes the UI reads. Raw RPC objects carry more fields than these; the
// interfaces name only what the interface actually renders, and every mapper
// in api.ts is tolerant of extras.

export type GuiMode = 'classic' | 'modern'

export interface ConnectionInfo {
  connected: boolean
  chain: string
  datadir: string
  managed: boolean
  wallet: string | null
}

export interface BlockchainInfo {
  chain: string
  blocks: number
  headers: number
  verificationprogress: number
}

export interface NetworkInfo {
  subversion: string
  connections: number
}

export interface ChainLockInfo {
  height: number
  blockhash: string
}

export interface NodeStatus {
  blockchain: BlockchainInfo
  network: NetworkInfo
  chainlock: ChainLockInfo | null
}

export interface Balances {
  trusted: number
  pending: number
  immature: number
  watchonly: number
}

export interface StakingExcluded {
  immature?: number
  bls?: number
  too_small?: number
  too_large?: number
  collateral?: number
  too_young?: number
  too_old?: number
}

export interface StakingInfo {
  staking: boolean
  errors: string
  weight: number
  netstakeweight: number
  expectedtime: number | null
  excluded: StakingExcluded | null
}

export interface WalletSummary {
  walletName: string
  encrypted: boolean
  unlockedUntil: number | null
  balances: Balances
  staking: StakingInfo | null
}

export type TxKind = 'received' | 'sent' | 'staking' | 'other'

export interface TxItem {
  txid: string
  kind: TxKind
  categoryRaw: string
  label: string
  address: string
  amount: number
  confirmations: number
  time: number
}

export interface MasternodeEntry {
  proTxHash: string
  address: string
  payee: string
  status: string
  posePenalty: number
  lastPaidHeight: number
  mine: boolean
}

export interface Proposal {
  hash: string
  name: string
  url: string
  paymentAmount: string
  yes: number
  no: number
  abstain: number
}

export interface CoinJoinInfo {
  enabled: boolean
  running: boolean
  rounds: number
  amount: number
  keysLeft: number
  sessions: number
}

export interface ReceiveAddress {
  address: string
  label: string
  amount: number
}
