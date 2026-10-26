# DeFCoN fixed seeds

`nodes_main.txt` lists the three DeFCoN mainnet bootstrap nodes on port 8192,
in the same order as `chainparams_seed_main` in
[`src/chainparamsseeds.h`](/src/chainparamsseeds.h). DNS seeders continue to
provide dynamic peer discovery.

`generate-seeds.py` uses only Python 3's standard library and reads
`nodes_main.txt`. It emits the mainnet BIP155 seed array. There is currently no
DeFCoN testnet, so no testnet seed list or array is generated; testnet fixed
seeds must be added and reviewed when that network launches. Devnet is separate.

From the repository root, generate a candidate and compare the serialized
mainnet bytes with the checked-in header:

```sh
candidate=$(mktemp)
python3 contrib/seeds/generate-seeds.py contrib/seeds > "$candidate"
python3 - "$candidate" src/chainparamsseeds.h <<'PY'
from pathlib import Path
import re
import sys

def mainnet_bytes(path):
    text = Path(path).read_text(encoding="utf8")
    body = text.split("chainparams_seed_main[] = {", 1)[1].split("};", 1)[0]
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", body))

generated, checked_in = map(mainnet_bytes, sys.argv[1:])
assert generated == checked_in, "mainnet seed bytes differ"
print(f"mainnet seed bytes match ({len(generated)} bytes)")
PY
rm "$candidate"
```

For a seed update, review the endpoints and serialized bytes together. Preserve
the hand-written comments in `src/chainparamsseeds.h`; do not replace that file
wholesale with the generated candidate. The current data alignment leaves the
compiled mainnet bytes unchanged.
