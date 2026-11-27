# RPC credentials

Use [rpcauth.py](rpcauth.py) to create an `rpcauth` entry for a DeFCoN JSON-RPC user. From the repository root, run:

```sh
python3 share/rpcauth/rpcauth.py operator
```

The script prints a salted password verifier and a generated password. Put only the line starting with `rpcauth=` in the node's [defcon.conf](../../doc/defcon-conf.md). Keep the password with the RPC client. To enter a password interactively, use `python3 share/rpcauth/rpcauth.py operator -`.
