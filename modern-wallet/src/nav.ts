import type { LucideIcon } from 'lucide-react'
import { Gauge, Gavel, History, Network, QrCode, Send, Settings, ShieldCheck } from 'lucide-react'

export type View = 'overview' | 'send' | 'receive' | 'transactions' | 'coinjoin' | 'masternodes' | 'governance' | 'settings'

export interface NavItem {
  view: View
  label: string
  icon: LucideIcon
}

export const navigation: NavItem[] = [
  { view: 'overview', label: 'Overview', icon: Gauge },
  { view: 'send', label: 'Send', icon: Send },
  { view: 'receive', label: 'Receive', icon: QrCode },
  { view: 'transactions', label: 'Transactions', icon: History },
  { view: 'coinjoin', label: 'CoinJoin', icon: ShieldCheck },
  { view: 'masternodes', label: 'Masternodes', icon: Network },
  { view: 'governance', label: 'Governance', icon: Gavel },
  { view: 'settings', label: 'Settings', icon: Settings },
]

export const viewCopy: Record<View, { eyebrow: string; title: string; caption: string }> = {
  overview: { eyebrow: 'Wallet command center', title: 'Overview', caption: 'Balances, activity and network health at a glance.' },
  send: { eyebrow: 'Create transaction', title: 'Send DFCN', caption: 'Review every detail before the Core wallet signs.' },
  receive: { eyebrow: 'Payment request', title: 'Receive DFCN', caption: 'Create a fresh address controlled by your Core wallet.' },
  transactions: { eyebrow: 'Wallet history', title: 'Transactions', caption: 'Search and inspect all wallet activity.' },
  coinjoin: { eyebrow: 'Privacy controls', title: 'CoinJoin', caption: 'Private balance and mixing status from the Core engine.' },
  masternodes: { eyebrow: 'Deterministic node list', title: 'Masternodes', caption: 'The network masternode list, live from the node.' },
  governance: { eyebrow: 'Network treasury', title: 'Governance', caption: 'Active proposals as the node sees them.' },
  settings: { eyebrow: 'Local preferences', title: 'Settings', caption: 'GUI mode, connection and wallet controls.' },
}
