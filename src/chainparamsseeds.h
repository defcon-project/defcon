#ifndef BITCOIN_CHAINPARAMSSEEDS_H
#define BITCOIN_CHAINPARAMSSEEDS_H

/**
 * Static DeFCoN mainnet bootstrap seeds.
 *
 * Keep this list intentionally small and limited to the three independently
 * monitored primary Seed Nodes. DNS seeders continue to provide dynamic peer
 * discovery, while this set makes a fresh AddrMan immediately bootstrappable.
 *
 * Each line is a BIP155 serialized (networkID, addr, port) tuple.
 */
static const uint8_t chainparams_seed_main[] = {
    // Seed 1: 154.12.247.198:8192
    0x01,0x04,0x9a,0x0c,0xf7,0xc6,0x20,0x00,
    // Seed 2: 154.12.247.214:8192
    0x01,0x04,0x9a,0x0c,0xf7,0xd6,0x20,0x00,
    // Seed 3: 154.26.155.66:8192
    0x01,0x04,0x9a,0x1a,0x9b,0x42,0x20,0x00,
};

#endif // BITCOIN_CHAINPARAMSSEEDS_H
