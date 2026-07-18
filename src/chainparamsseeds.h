#ifndef BITCOIN_CHAINPARAMSSEEDS_H
#define BITCOIN_CHAINPARAMSSEEDS_H

/**
 * Static DeFCoN mainnet bootstrap seeds.
 *
 * Keep this list intentionally small and limited to the two independently
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
};

static const uint8_t chainparams_seed_test[] = {
    0x01,0x04,0x2b,0xe5,0x4d,0x2e,0x4e,0x1f,
    0x01,0x04,0x2d,0x4d,0xa7,0xf7,0x4e,0x1f,
    0x01,0x04,0xb2,0x3e,0xcb,0xf9,0x4e,0x1f,
};

#endif // BITCOIN_CHAINPARAMSSEEDS_H
